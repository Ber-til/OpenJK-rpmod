/*
===========================================================================
Copyright (C) 2026, OpenJK contributors

This file is part of the OpenJK source code.

OpenJK is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License version 2 as
published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, see <http://www.gnu.org/licenses/>.
===========================================================================
*/

// vk_rpmod_post.cpp -- OpenJK-rpmod: what Vulkan adds to the world's main view, beyond TaystJK's
// renderer: the cinematic camera's depth of field and filter (as rd-vanilla draws them), and
// ambient occlusion, soft particles, sun shafts, motion blur, colour grading and tone mapping,
// FXAA and sharpening.
//
//   the solid surfaces drawn  the view's depth as distances, into an image of its own; ambient
//                             occlusion from them, multiplied into the view
//   the see-through ones      effects (sprites, effect polys) faded where they meet what's behind,
//                             by a shader of our own that reads the distances (soft particles)
//   after dynamic glow        the passes over the view, between two images of the screen's size:
//                             sun shafts, motion blur, depth of field, grading, the cinematic
//                             filter, FXAA, sharpening; then the result back into the view
//
// Only with the post effects (r_fbo 1): the view is drawn into an image the passes can read. The
// shaders are glsl/rpmod/*, compiled by shaders/compile_rpmod.bat into shader_data_rpmod.c.

#include "tr_local.h"
#include "shaders/spirv/shader_data_rpmod.c"

static cvar_t	*r_ssao;
static cvar_t	*r_ssaoRadius;
static cvar_t	*r_ssaoIntensity;
static cvar_t	*r_softParticles;
static cvar_t	*r_softParticlesDepth;
static cvar_t	*r_fxaa;
static cvar_t	*r_sharpen;
static cvar_t	*r_relief, *r_reliefStrength, *r_reliefSpecular;
static cvar_t	*r_volumetric, *r_volumetricIntensity, *r_volumetricFog;
static cvar_t	*r_ssr, *r_ssrStrength;
static cvar_t	*r_shadowMaps, *r_shadowDistance, *r_shadowStrength, *r_shadowSoftness, *r_shadowSun;

#define RP_SHADOW_SIZE		2048		// the shadow map's width and height
#define RP_SHADOW_RANGE		4096.0f		// how deep it reaches along the light
#define RP_MAX_CASTERS		8192
static cvar_t	*r_tonemap;
static cvar_t	*r_exposure;
static cvar_t	*r_saturation;
static cvar_t	*r_contrast;
static cvar_t	*r_warmth;
static cvar_t	*r_mapGrades;
static cvar_t	*r_sunShafts;
static cvar_t	*r_sunShaftsIntensity;
static cvar_t	*r_motionBlur;
static cvar_t	*r_dofBokeh;

static struct {
	qboolean			usable;			// the depth buffer can be read by a shader
	qboolean			ready;			// everything is there for this window

	VkShaderModule		depthFs, depthMsFs, dofFs, filterFs, copyFs;
	VkShaderModule		ssaoFs, aoBlurFs, aoApplyFs, shaftsFs[3], mblurFs, gradeFs, fxaaFs, casFs, volumFs, ssrFs, shadowFs;
	VkShaderModule		softFs[6];		// in pairs, unfogged then fogged: vk_create_pipeline takes the next one for fog
	VkPipelineLayout	layout;			// two image sets, 128 bytes of push constants

	// the screen's size: two images the passes go between, the distances, ambient occlusion
	VkImage				postImage[2];
	VkImageView			postView[2];
	VkDeviceMemory		postMemory[2];
	VkImage				distImage;
	VkImageView			distView;
	VkDeviceMemory		distMemory;
	VkImage				aoImage[2];
	VkImageView			aoView[2];
	VkDeviceMemory		aoMemory[2];
	VkImageView			depthView;		// the depth buffer's depth, to read

	// the shadow map: the models' depth from the light, and a colour image the pass needs beside it
	VkFormat			shadowFormat;
	VkImage				shadowImage, shadowColor;
	VkImageView			shadowView, shadowColorView;
	VkDeviceMemory		shadowMemory, shadowColorMemory;
	VkRenderPass		shadowPass;
	VkFramebuffer		shadowFb;
	VkDescriptorSet		shadowSet;

	VkDescriptorSet		sceneSet, postSet[2], distSet, depthSet, aoSet[2];
	qboolean			setsAllocated;

	VkRenderPass		postPass, distPass, aoPass;
	VkFramebuffer		postFb[2], distFb, aoFb[2];

	VkPipeline			depthPipe, dofPipe, filterPipe, copyPipe;
	VkPipeline			ssaoPipe, aoBlurPipe, aoApplyPipe, shaftsPipe[3], mblurPipe, gradePipe, fxaaPipe, casPipe, volumPipe, ssrPipe, shadowPipe;

	qboolean			haveDistance;	// this view's distances are in distImage
	qboolean			softOn;			// and effects drawn now are faded by them

	// the main view's camera the frame before, for motion blur
	qboolean			prevValid;
	vec3_t				prevOrigin;
	vec3_t				prevAxis[3];
	int					prevTime;
	VkRect2D			prevRect;
} rp;

#define RP_NUM_SETS 8	// sceneSet, postSet[2], distSet, depthSet, aoSet[2], shadowSet
#if RP_NUM_SETS > VK_RPMOD_SETS
#error the descriptor pool has to have room for these sets
#endif

// a map's own look (maps/<name>.grade), in place of r_exposure and the rest while it's loaded
static struct {
	qboolean	valid;
	float		exposure, saturation, contrast, warmth;
	int			tonemap;
} mapGrade;

/*
===============
the settings
===============
*/
void vk_rpmod_register_cvars( void ) {
	const char *fbo = "\nRequires " S_COLOR_CYAN "\\r_fbo 1";

	r_ssao = ri.Cvar_Get( "r_ssao", "0", CVAR_ARCHIVE_ND, va( "Ambient occlusion: corners, creases and the ground under things darkened%s", fbo ) );
	ri.Cvar_CheckRange( r_ssao, 0, 1, qtrue );
	r_ssaoRadius = ri.Cvar_Get( "r_ssaoRadius", "48", CVAR_ARCHIVE_ND, "How far ambient occlusion reaches, in world units" );
	ri.Cvar_CheckRange( r_ssaoRadius, 4, 256, qfalse );
	r_ssaoIntensity = ri.Cvar_Get( "r_ssaoIntensity", "0.6", CVAR_ARCHIVE_ND, "How dark ambient occlusion makes things: 0.3 low, 0.6 medium, 1 strong" );
	ri.Cvar_CheckRange( r_ssaoIntensity, 0, 4, qfalse );
	r_softParticles = ri.Cvar_Get( "r_softParticles", "0", CVAR_ARCHIVE_ND, va( "Soft particles: smoke, fire and other effects fade where they meet walls and floors instead of cutting into them%s", fbo ) );
	ri.Cvar_CheckRange( r_softParticles, 0, 1, qtrue );
	r_softParticlesDepth = ri.Cvar_Get( "r_softParticlesDepth", "24", CVAR_ARCHIVE_ND, "How far in front of a wall soft particles start to fade, in world units" );
	ri.Cvar_CheckRange( r_softParticlesDepth, 1, 256, qtrue );
	r_fxaa = ri.Cvar_Get( "r_fxaa", "0", CVAR_ARCHIVE_ND, va( "FXAA: smooths edges over the finished view, those of grass and fences too%s", fbo ) );
	ri.Cvar_CheckRange( r_fxaa, 0, 1, qtrue );
	r_relief = ri.Cvar_Get( "r_relief", "0", CVAR_ARCHIVE_ND | CVAR_LATCH, "Surface relief: dynamic lights and lit sabers pick out the relief of stone, metal and cloth, worked out from each texture (needs \\r_dlightMode 2; \\vid_restart to change)" );
	ri.Cvar_CheckRange( r_relief, 0, 1, qtrue );
	r_reliefStrength = ri.Cvar_Get( "r_reliefStrength", "8", CVAR_ARCHIVE_ND, "Surface relief: how deep it is, in world units" );
	ri.Cvar_CheckRange( r_reliefStrength, 0, 64, qfalse );
	r_reliefSpecular = ri.Cvar_Get( "r_reliefSpecular", "0.35", CVAR_ARCHIVE_ND, "Surface relief: how much the lit surfaces shine, 0 none" );
	ri.Cvar_CheckRange( r_reliefSpecular, 0, 2, qfalse );
	r_volumetric = ri.Cvar_Get( "r_volumetric", "0", CVAR_ARCHIVE_ND, va( "Volumetric light: sabers, blaster bolts, torches and other dynamic lights glow in the air around them, more in a map's fog%s", fbo ) );
	ri.Cvar_CheckRange( r_volumetric, 0, 1, qtrue );
	r_volumetricIntensity = ri.Cvar_Get( "r_volumetricIntensity", "0.5", CVAR_ARCHIVE_ND, "Volumetric light: how thick the air is, how much the lights glow in it" );
	ri.Cvar_CheckRange( r_volumetricIntensity, 0, 4, qfalse );
	r_volumetricFog = ri.Cvar_Get( "r_volumetricFog", "2", CVAR_ARCHIVE_ND, "Volumetric light: how many times more the lights glow on a map with fog all over" );
	ri.Cvar_CheckRange( r_volumetricFog, 1, 8, qfalse );
	r_ssr = ri.Cvar_Get( "r_ssr", "0", CVAR_ARCHIVE_ND, va( "Reflections: smooth floors mirror what's on screen, polished ones most%s", fbo ) );
	ri.Cvar_CheckRange( r_ssr, 0, 1, qtrue );
	r_ssrStrength = ri.Cvar_Get( "r_ssrStrength", "0.8", CVAR_ARCHIVE_ND, "Reflections: how strongly floors mirror, 0 to 2" );
	ri.Cvar_CheckRange( r_ssrStrength, 0, 2, qfalse );
	r_shadowMaps = ri.Cvar_Get( "r_shadowMaps", "0", CVAR_ARCHIVE_ND, va( "Real shadows: players, NPCs and models cast soft shadows on the map and on each other, in place of the blob under them%s", fbo ) );
	ri.Cvar_CheckRange( r_shadowMaps, 0, 1, qtrue );
	r_shadowDistance = ri.Cvar_Get( "r_shadowDistance", "768", CVAR_ARCHIVE_ND, "Real shadows: how far from you they're drawn, in world units; nearer is sharper" );
	ri.Cvar_CheckRange( r_shadowDistance, 256, 2048, qfalse );
	r_shadowStrength = ri.Cvar_Get( "r_shadowStrength", "0.5", CVAR_ARCHIVE_ND, "Real shadows: how dark they are, 0 to 1" );
	ri.Cvar_CheckRange( r_shadowStrength, 0, 1, qfalse );
	r_shadowSoftness = ri.Cvar_Get( "r_shadowSoftness", "2", CVAR_ARCHIVE_ND, "Real shadows: how soft their edges are, 0 hard" );
	ri.Cvar_CheckRange( r_shadowSoftness, 0, 8, qfalse );
	r_shadowSun = ri.Cvar_Get( "r_shadowSun", "0", CVAR_ARCHIVE_ND, "Real shadows: 0 cast nearly straight down, which suits indoors; 1 from the map's sun, on maps whose sky has one" );
	ri.Cvar_CheckRange( r_shadowSun, 0, 1, qtrue );
	r_sharpen = ri.Cvar_Get( "r_sharpen", "0", CVAR_ARCHIVE_ND, va( "Sharpening, from 0 (off) to 1%s", fbo ) );
	ri.Cvar_CheckRange( r_sharpen, 0, 1, qfalse );
	r_tonemap = ri.Cvar_Get( "r_tonemap", "0", CVAR_ARCHIVE_ND, va( "Tone mapping: bright light rolled off instead of clipping to white. 1 soft: the rest of the picture as it is; 2 film (ACES): darker shadows too%s", fbo ) );
	ri.Cvar_CheckRange( r_tonemap, 0, 2, qtrue );
	r_exposure = ri.Cvar_Get( "r_exposure", "1", CVAR_ARCHIVE_ND, va( "Colour grading: the view's light scaled by this%s", fbo ) );
	ri.Cvar_CheckRange( r_exposure, 0.25f, 4, qfalse );
	r_saturation = ri.Cvar_Get( "r_saturation", "1", CVAR_ARCHIVE_ND, va( "Colour grading: 0 grey, 1 as it is, more for stronger colours%s", fbo ) );
	ri.Cvar_CheckRange( r_saturation, 0, 2, qfalse );
	r_contrast = ri.Cvar_Get( "r_contrast", "1", CVAR_ARCHIVE_ND, va( "Colour grading: 1 as it is, more for more contrast%s", fbo ) );
	ri.Cvar_CheckRange( r_contrast, 0.5f, 2, qfalse );
	r_warmth = ri.Cvar_Get( "r_warmth", "0", CVAR_ARCHIVE_ND, va( "Colour grading: -1 cold (blue) to 1 warm (orange)%s", fbo ) );
	ri.Cvar_CheckRange( r_warmth, -1, 1, qfalse );
	r_mapGrades = ri.Cvar_Get( "r_mapGrades", "1", CVAR_ARCHIVE_ND, "A map's own colour grading (maps/<map>.grade) in place of yours while it's loaded" );
	ri.Cvar_CheckRange( r_mapGrades, 0, 1, qtrue );
	r_sunShafts = ri.Cvar_Get( "r_sunShafts", "0", CVAR_ARCHIVE_ND, va( "Sun shafts: beams of light from the sun through gaps, on maps whose sky has a sun%s", fbo ) );
	ri.Cvar_CheckRange( r_sunShafts, 0, 1, qtrue );
	r_sunShaftsIntensity = ri.Cvar_Get( "r_sunShaftsIntensity", "1", CVAR_ARCHIVE_ND, "How bright sun shafts are" );
	ri.Cvar_CheckRange( r_sunShaftsIntensity, 0, 4, qfalse );
	r_motionBlur = ri.Cvar_Get( "r_motionBlur", "0", CVAR_ARCHIVE_ND, va( "Motion blur when the camera moves, from 0 (off) to 1%s", fbo ) );
	ri.Cvar_CheckRange( r_motionBlur, 0, 1, qfalse );
	r_dofBokeh = ri.Cvar_Get( "r_dofBokeh", "1", CVAR_ARCHIVE_ND, "Bokeh in the cinematic camera's depth of field: bright points blur into round discs, from 0 (off, as in OpenGL) to 1" );
	ri.Cvar_CheckRange( r_dofBokeh, 0, 1, qfalse );
}

/*
===============
a map's own grading: maps/<name>.grade, lines of "exposure 1.2", "saturation 0.9", "contrast 1.1",
"warmth 0.3", "tonemap 1" (or 2, film)
===============
*/
void vk_rpmod_map_grade( const char *bspName ) {
	char name[MAX_QPATH];
	char *buf;
	const char *p, *token;

	Com_Memset( &mapGrade, 0, sizeof( mapGrade ) );
	COM_StripExtension( bspName, name, sizeof( name ) );
	Q_strcat( name, sizeof( name ), ".grade" );
	if ( ri.FS_ReadFile( name, (void **)&buf ) <= 0 || !buf )
		return;

	mapGrade.exposure = mapGrade.saturation = mapGrade.contrast = 1.0f;
	p = buf;
	while ( 1 ) {
		char key[64];

		token = COM_ParseExt( &p, qtrue );
		if ( !token[0] )
			break;
		Q_strncpyz( key, token, sizeof( key ) );
		token = COM_ParseExt( &p, qfalse );
		if ( !Q_stricmp( key, "exposure" ) )
			mapGrade.exposure = Com_Clamp( 0.25f, 4.0f, atof( token ) );
		else if ( !Q_stricmp( key, "saturation" ) )
			mapGrade.saturation = Com_Clamp( 0.0f, 2.0f, atof( token ) );
		else if ( !Q_stricmp( key, "contrast" ) )
			mapGrade.contrast = Com_Clamp( 0.5f, 2.0f, atof( token ) );
		else if ( !Q_stricmp( key, "warmth" ) )
			mapGrade.warmth = Com_Clamp( -1.0f, 1.0f, atof( token ) );
		else if ( !Q_stricmp( key, "tonemap" ) )
			mapGrade.tonemap = Com_Clampi( 0, 2, atoi( token ) );
	}
	ri.FS_FreeFile( buf );
	mapGrade.valid = qtrue;
	ri.Printf( PRINT_DEVELOPER, "colour grading from %s\n", name );
}

static VkShaderModule RP_ShaderModule( const unsigned char *bytes, size_t size, const char *name ) {
	VkShaderModuleCreateInfo desc;
	VkShaderModule module;

	Com_Memset( &desc, 0, sizeof( desc ) );
	desc.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	desc.codeSize = size;
	desc.pCode = (const uint32_t *)bytes;
	VK_CHECK( qvkCreateShaderModule( vk.device, &desc, NULL, &module ) );
	VK_SET_OBJECT_NAME( module, name, VK_DEBUG_REPORT_OBJECT_TYPE_SHADER_MODULE_EXT );
	return module;
}

#define RP_MODULE( m, data, name ) m = RP_ShaderModule( data, sizeof( data ), name )

// surface relief (r_relief, taken at vid_restart): the per-pixel lights' fragment shaders swapped for
// the same with relief (light_frag.tmpl built with USE_RELIEF)
void vk_rpmod_light_modules( void ) {
	if ( !r_relief || !r_relief->integer )
		return;
	for ( int i = 0; i < 2; i++ ) {
		for ( int j = 0; j < 2; j++ ) {
			if ( vk.shaders.frag.light[i][j] != VK_NULL_HANDLE )
				qvkDestroyShaderModule( vk.device, vk.shaders.frag.light[i][j], NULL );
		}
	}
	RP_MODULE( vk.shaders.frag.light[0][0], rpmod_light_relief_frag_spv, "rpmod relief light" );
	RP_MODULE( vk.shaders.frag.light[0][1], rpmod_light_relief_fog_frag_spv, "rpmod relief light fog" );
	RP_MODULE( vk.shaders.frag.light[1][0], rpmod_light_relief_line_frag_spv, "rpmod relief linear light" );
	RP_MODULE( vk.shaders.frag.light[1][1], rpmod_light_relief_line_fog_frag_spv, "rpmod relief linear light fog" );
	ri.Printf( PRINT_ALL, "Surface relief on (r_relief)%s\n", r_dlightMode->integer == 2 ? "" : ": it needs r_dlightMode 2" );
}

// what the relief shader reads in the light's otherwise unused w: its depth, and the shine
void vk_rpmod_light_params( float *depth, float *shine ) {
	// a latched cvar keeps the value it started with until vid_restart, as the shaders do
	const qboolean on = (qboolean)( r_relief && r_relief->integer );

	*depth = on ? r_reliefStrength->value : 0.0f;
	*shine = on ? r_reliefSpecular->value : 0.0f;
}

// the depth buffer read by a shader: its format has to allow it
qboolean vk_rpmod_depth_readable( void ) {
	VkFormatProperties props;

	qvkGetPhysicalDeviceFormatProperties( vk.physical_device, vk.depth_format, &props );
	return (qboolean)( ( props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT ) != 0 );
}

/*
===============
once: the shaders and the pipeline layout
===============
*/
void vk_rpmod_init( void ) {
	VkDescriptorSetLayout setLayouts[2] = { vk.set_layout_sampler, vk.set_layout_sampler };
	VkPipelineLayoutCreateInfo desc;
	VkPushConstantRange push;

	Com_Memset( &rp, 0, sizeof( rp ) );

	RP_MODULE( rp.depthFs, rpmod_depth_frag_spv, "rpmod depth to distance" );
	RP_MODULE( rp.depthMsFs, rpmod_depth_ms_frag_spv, "rpmod depth to distance msaa" );
	RP_MODULE( rp.dofFs, rpmod_dof_frag_spv, "rpmod depth of field" );
	RP_MODULE( rp.filterFs, rpmod_filter_frag_spv, "rpmod screen filter" );
	RP_MODULE( rp.copyFs, rpmod_copy_frag_spv, "rpmod copy" );
	RP_MODULE( rp.ssaoFs, rpmod_ssao_frag_spv, "rpmod ambient occlusion" );
	RP_MODULE( rp.aoBlurFs, rpmod_aoblur_frag_spv, "rpmod ambient occlusion blur" );
	RP_MODULE( rp.aoApplyFs, rpmod_aoapply_frag_spv, "rpmod ambient occlusion apply" );
	RP_MODULE( rp.softFs[0], rpmod_soft_gen_frag_spv, "rpmod soft particles" );
	RP_MODULE( rp.softFs[1], rpmod_soft_gen_fog_frag_spv, "rpmod soft particles fogged" );
	RP_MODULE( rp.softFs[2], rpmod_soft_ident_frag_spv, "rpmod soft particles identity" );
	RP_MODULE( rp.softFs[3], rpmod_soft_ident_fog_frag_spv, "rpmod soft particles identity fogged" );
	RP_MODULE( rp.softFs[4], rpmod_soft_fixed_frag_spv, "rpmod soft particles fixed colour" );
	RP_MODULE( rp.softFs[5], rpmod_soft_fixed_fog_frag_spv, "rpmod soft particles fixed colour fogged" );
	RP_MODULE( rp.shaftsFs[0], rpmod_shafts_mask_frag_spv, "rpmod sun shafts mask" );
	RP_MODULE( rp.shaftsFs[1], rpmod_shafts_blur_frag_spv, "rpmod sun shafts blur" );
	RP_MODULE( rp.shaftsFs[2], rpmod_shafts_add_frag_spv, "rpmod sun shafts add" );
	RP_MODULE( rp.mblurFs, rpmod_mblur_frag_spv, "rpmod motion blur" );
	RP_MODULE( rp.gradeFs, rpmod_grade_frag_spv, "rpmod colour grading" );
	RP_MODULE( rp.fxaaFs, rpmod_fxaa_frag_spv, "rpmod fxaa" );
	RP_MODULE( rp.casFs, rpmod_cas_frag_spv, "rpmod sharpen" );
	RP_MODULE( rp.volumFs, rpmod_volum_frag_spv, "rpmod volumetric light" );
	RP_MODULE( rp.ssrFs, rpmod_ssr_frag_spv, "rpmod reflections" );
	RP_MODULE( rp.shadowFs, rpmod_shadow_frag_spv, "rpmod real shadows" );

	push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	push.offset = 0;
	push.size = 128;
	Com_Memset( &desc, 0, sizeof( desc ) );
	desc.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	desc.setLayoutCount = ARRAY_LEN( setLayouts );
	desc.pSetLayouts = setLayouts;
	desc.pushConstantRangeCount = 1;
	desc.pPushConstantRanges = &push;
	VK_CHECK( qvkCreatePipelineLayout( vk.device, &desc, NULL, &rp.layout ) );
	VK_SET_OBJECT_NAME( rp.layout, "pipeline layout - rpmod post", VK_DEBUG_REPORT_OBJECT_TYPE_PIPELINE_LAYOUT_EXT );
}

void vk_rpmod_shutdown( void ) {
	VkShaderModule *modules[] = { &rp.depthFs, &rp.depthMsFs, &rp.dofFs, &rp.filterFs, &rp.copyFs,
		&rp.ssaoFs, &rp.aoBlurFs, &rp.aoApplyFs, &rp.softFs[0], &rp.softFs[1], &rp.softFs[2], &rp.softFs[3], &rp.softFs[4], &rp.softFs[5],
		&rp.shaftsFs[0], &rp.shaftsFs[1], &rp.shaftsFs[2], &rp.mblurFs, &rp.gradeFs, &rp.fxaaFs, &rp.casFs, &rp.volumFs, &rp.ssrFs, &rp.shadowFs };

	for ( size_t i = 0; i < ARRAY_LEN( modules ); i++ ) {
		if ( *modules[i] != VK_NULL_HANDLE )
			qvkDestroyShaderModule( vk.device, *modules[i], NULL );
		*modules[i] = VK_NULL_HANDLE;
	}
	if ( rp.layout != VK_NULL_HANDLE )
		qvkDestroyPipelineLayout( vk.device, rp.layout, NULL );
	rp.layout = VK_NULL_HANDLE;
	// the sets go with the descriptor pool
	rp.setsAllocated = qfalse;
}

/*
===============
the images, with the window's other attachments
===============
*/
static void RP_CreateImage( VkFormat format, VkImage *image, VkImageView *view, VkDeviceMemory *memory, const char *name ) {
	VkImageCreateInfo desc;
	VkMemoryRequirements req;
	VkMemoryAllocateInfo alloc;
	VkImageViewCreateInfo vdesc;

	Com_Memset( &desc, 0, sizeof( desc ) );
	desc.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	desc.imageType = VK_IMAGE_TYPE_2D;
	desc.format = format;
	desc.extent.width = glConfig.vidWidth;
	desc.extent.height = glConfig.vidHeight;
	desc.extent.depth = 1;
	desc.mipLevels = 1;
	desc.arrayLayers = 1;
	desc.samples = VK_SAMPLE_COUNT_1_BIT;
	desc.tiling = VK_IMAGE_TILING_OPTIMAL;
	desc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	desc.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	desc.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VK_CHECK( qvkCreateImage( vk.device, &desc, NULL, image ) );

	qvkGetImageMemoryRequirements( vk.device, *image, &req );
	Com_Memset( &alloc, 0, sizeof( alloc ) );
	alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc.allocationSize = req.size;
	alloc.memoryTypeIndex = vk_find_memory_type( req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT );
	VK_CHECK( qvkAllocateMemory( vk.device, &alloc, NULL, memory ) );
	VK_CHECK( qvkBindImageMemory( vk.device, *image, *memory, 0 ) );

	Com_Memset( &vdesc, 0, sizeof( vdesc ) );
	vdesc.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	vdesc.image = *image;
	vdesc.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vdesc.format = format;
	vdesc.components.r = vdesc.components.g = vdesc.components.b = vdesc.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
	vdesc.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	vdesc.subresourceRange.levelCount = 1;
	vdesc.subresourceRange.layerCount = 1;
	VK_CHECK( qvkCreateImageView( vk.device, &vdesc, NULL, view ) );

	VK_SET_OBJECT_NAME( *image, name, VK_DEBUG_REPORT_OBJECT_TYPE_IMAGE_EXT );
	VK_SET_OBJECT_NAME( *view, name, VK_DEBUG_REPORT_OBJECT_TYPE_IMAGE_VIEW_EXT );
}

static void RP_DestroyImage( VkImage *image, VkImageView *view, VkDeviceMemory *memory ) {
	if ( *view != VK_NULL_HANDLE )
		qvkDestroyImageView( vk.device, *view, NULL );
	if ( *image != VK_NULL_HANDLE )
		qvkDestroyImage( vk.device, *image, NULL );
	if ( memory && *memory != VK_NULL_HANDLE )
		qvkFreeMemory( vk.device, *memory, NULL );
	*view = VK_NULL_HANDLE;
	*image = VK_NULL_HANDLE;
	if ( memory )
		*memory = VK_NULL_HANDLE;
}

static void RP_SetImage( VkDescriptorSet set, VkImageView view, VkImageLayout layout, int filter ) {
	VkDescriptorImageInfo info;
	VkWriteDescriptorSet desc;
	Vk_Sampler_Def sd;

	Com_Memset( &sd, 0, sizeof( sd ) );
	sd.gl_mag_filter = sd.gl_min_filter = filter;
	sd.address_mode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sd.max_lod_1_0 = qtrue;
	sd.noAnisotropy = qtrue;

	info.sampler = vk_find_sampler( &sd );
	info.imageView = view;
	info.imageLayout = layout;

	Com_Memset( &desc, 0, sizeof( desc ) );
	desc.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	desc.dstSet = set;
	desc.dstBinding = 0;
	desc.descriptorCount = 1;
	desc.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	desc.pImageInfo = &info;
	qvkUpdateDescriptorSets( vk.device, 1, &desc, 0, NULL );
}

/*
===============
the image sets: from the descriptor pool, which every map load empties (vk_release_resources), so
taken again with the renderer's own in vk_init_descriptors, and pointed at the images whenever
the renderer points its own (vk_update_attachment_descriptors: new window size, new samplers)
===============
*/
void vk_rpmod_init_descriptors( void ) {
	VkDescriptorSetAllocateInfo alloc;
	VkDescriptorSet *sets[RP_NUM_SETS] = { &rp.sceneSet, &rp.postSet[0], &rp.postSet[1], &rp.distSet, &rp.depthSet, &rp.aoSet[0], &rp.aoSet[1], &rp.shadowSet };

	Com_Memset( &alloc, 0, sizeof( alloc ) );
	alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	alloc.descriptorPool = vk.descriptor_pool;
	alloc.descriptorSetCount = 1;
	alloc.pSetLayouts = &vk.set_layout_sampler;
	for ( int i = 0; i < RP_NUM_SETS; i++ )
		VK_CHECK( qvkAllocateDescriptorSets( vk.device, &alloc, sets[i] ) );
	rp.setsAllocated = qtrue;
	vk_rpmod_update_descriptors();
}

// the descriptor pool emptied: the sets are gone with it
void vk_rpmod_forget_descriptors( void ) {
	rp.setsAllocated = qfalse;
	rp.sceneSet = rp.postSet[0] = rp.postSet[1] = rp.distSet = rp.depthSet = rp.aoSet[0] = rp.aoSet[1] = rp.shadowSet = VK_NULL_HANDLE;
	rp.haveDistance = rp.softOn = qfalse;
}

void vk_rpmod_update_descriptors( void ) {
	if ( !rp.setsAllocated || rp.postView[0] == VK_NULL_HANDLE )
		return;
	RP_SetImage( rp.sceneSet, vk.color_image_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, GL_LINEAR );
	RP_SetImage( rp.postSet[0], rp.postView[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, GL_LINEAR );
	RP_SetImage( rp.postSet[1], rp.postView[1], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, GL_LINEAR );
	RP_SetImage( rp.distSet, rp.distView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, GL_NEAREST );
	RP_SetImage( rp.aoSet[0], rp.aoView[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, GL_NEAREST );
	RP_SetImage( rp.aoSet[1], rp.aoView[1], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, GL_NEAREST );
	if ( rp.depthView != VK_NULL_HANDLE )
		RP_SetImage( rp.depthSet, rp.depthView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, GL_NEAREST );
	if ( rp.shadowView != VK_NULL_HANDLE )
		RP_SetImage( rp.shadowSet, rp.shadowView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, GL_NEAREST );
}

/*
===============
the shadow map's images: its depth, read after; a colour image only because the pass's pipelines are
the renderer's own, which draw colour too
===============
*/
static VkFormat RP_ShadowFormat( void ) {
	const VkFormat formats[] = { VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM };
	const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;

	for ( size_t i = 0; i < ARRAY_LEN( formats ); i++ ) {
		VkFormatProperties props;

		qvkGetPhysicalDeviceFormatProperties( vk.physical_device, formats[i], &props );
		if ( ( props.optimalTilingFeatures & need ) == need )
			return formats[i];
	}
	return VK_FORMAT_UNDEFINED;
}

static void RP_CreateShadowImage( VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, VkImage *image, VkImageView *view, VkDeviceMemory *memory, const char *name ) {
	VkImageCreateInfo desc;
	VkMemoryRequirements req;
	VkMemoryAllocateInfo alloc;
	VkImageViewCreateInfo vdesc;

	Com_Memset( &desc, 0, sizeof( desc ) );
	desc.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	desc.imageType = VK_IMAGE_TYPE_2D;
	desc.format = format;
	desc.extent.width = desc.extent.height = RP_SHADOW_SIZE;
	desc.extent.depth = 1;
	desc.mipLevels = 1;
	desc.arrayLayers = 1;
	desc.samples = VK_SAMPLE_COUNT_1_BIT;
	desc.tiling = VK_IMAGE_TILING_OPTIMAL;
	desc.usage = usage;
	desc.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	desc.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VK_CHECK( qvkCreateImage( vk.device, &desc, NULL, image ) );

	qvkGetImageMemoryRequirements( vk.device, *image, &req );
	Com_Memset( &alloc, 0, sizeof( alloc ) );
	alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc.allocationSize = req.size;
	alloc.memoryTypeIndex = vk_find_memory_type( req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT );
	VK_CHECK( qvkAllocateMemory( vk.device, &alloc, NULL, memory ) );
	VK_CHECK( qvkBindImageMemory( vk.device, *image, *memory, 0 ) );

	Com_Memset( &vdesc, 0, sizeof( vdesc ) );
	vdesc.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	vdesc.image = *image;
	vdesc.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vdesc.format = format;
	vdesc.components.r = vdesc.components.g = vdesc.components.b = vdesc.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
	vdesc.subresourceRange.aspectMask = aspect;
	vdesc.subresourceRange.levelCount = 1;
	vdesc.subresourceRange.layerCount = 1;
	VK_CHECK( qvkCreateImageView( vk.device, &vdesc, NULL, view ) );

	VK_SET_OBJECT_NAME( *image, name, VK_DEBUG_REPORT_OBJECT_TYPE_IMAGE_EXT );
	VK_SET_OBJECT_NAME( *view, name, VK_DEBUG_REPORT_OBJECT_TYPE_IMAGE_VIEW_EXT );
}

VkRenderPass vk_rpmod_shadow_pass( void ) {
	return rp.shadowPass;
}

uint32_t vk_rpmod_shadow_size( void ) {
	return RP_SHADOW_SIZE;
}

void vk_rpmod_create_attachments( void ) {
	VkImageViewCreateInfo vdesc;
	VkCommandBuffer cmd;
	VkImage *images[] = { &rp.postImage[0], &rp.postImage[1], &rp.distImage, &rp.aoImage[0], &rp.aoImage[1] };

	rp.ready = qfalse;
	rp.usable = qfalse;
	if ( !vk.fboActive || vk.depth_image == VK_NULL_HANDLE )
		return;
	rp.usable = vk_rpmod_depth_readable();

	RP_CreateImage( vk.color_format, &rp.postImage[0], &rp.postView[0], &rp.postMemory[0], "rpmod post 0" );
	RP_CreateImage( vk.color_format, &rp.postImage[1], &rp.postView[1], &rp.postMemory[1], "rpmod post 1" );
	RP_CreateImage( VK_FORMAT_R32_SFLOAT, &rp.distImage, &rp.distView, &rp.distMemory, "rpmod distance" );
	RP_CreateImage( VK_FORMAT_R8_UNORM, &rp.aoImage[0], &rp.aoView[0], &rp.aoMemory[0], "rpmod ambient occlusion 0" );
	RP_CreateImage( VK_FORMAT_R8_UNORM, &rp.aoImage[1], &rp.aoView[1], &rp.aoMemory[1], "rpmod ambient occlusion 1" );
	rp.shadowFormat = RP_ShadowFormat();
	if ( rp.shadowFormat != VK_FORMAT_UNDEFINED ) {
		RP_CreateShadowImage( rp.shadowFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT,
			&rp.shadowImage, &rp.shadowView, &rp.shadowMemory, "rpmod shadow map" );
		RP_CreateShadowImage( VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
			&rp.shadowColor, &rp.shadowColorView, &rp.shadowColorMemory, "rpmod shadow map colour" );
	}

	// as the render passes have them before and after
	cmd = vk_begin_command_buffer();
	for ( size_t i = 0; i < ARRAY_LEN( images ); i++ )
		vk_record_image_layout_transition( cmd, *images[i], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 0 );
	if ( rp.shadowImage != VK_NULL_HANDLE )
		vk_record_image_layout_transition( cmd, rp.shadowImage, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 0 );
	vk_end_command_buffer( cmd, __func__ );

	// the depth buffer's depth, without its stencil
	if ( rp.usable ) {
		Com_Memset( &vdesc, 0, sizeof( vdesc ) );
		vdesc.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		vdesc.image = vk.depth_image;
		vdesc.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vdesc.format = vk.depth_format;
		vdesc.components.r = vdesc.components.g = vdesc.components.b = vdesc.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
		vdesc.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		vdesc.subresourceRange.levelCount = 1;
		vdesc.subresourceRange.layerCount = 1;
		VK_CHECK( qvkCreateImageView( vk.device, &vdesc, NULL, &rp.depthView ) );
		VK_SET_OBJECT_NAME( rp.depthView, "rpmod depth read", VK_DEBUG_REPORT_OBJECT_TYPE_IMAGE_VIEW_EXT );
	}
	vk_rpmod_update_descriptors();
}

void vk_rpmod_destroy_attachments( void ) {
	rp.ready = qfalse;
	if ( rp.depthView != VK_NULL_HANDLE )
		qvkDestroyImageView( vk.device, rp.depthView, NULL );
	rp.depthView = VK_NULL_HANDLE;
	RP_DestroyImage( &rp.postImage[0], &rp.postView[0], &rp.postMemory[0] );
	RP_DestroyImage( &rp.postImage[1], &rp.postView[1], &rp.postMemory[1] );
	RP_DestroyImage( &rp.distImage, &rp.distView, &rp.distMemory );
	RP_DestroyImage( &rp.aoImage[0], &rp.aoView[0], &rp.aoMemory[0] );
	RP_DestroyImage( &rp.aoImage[1], &rp.aoView[1], &rp.aoMemory[1] );
	RP_DestroyImage( &rp.shadowImage, &rp.shadowView, &rp.shadowMemory );
	RP_DestroyImage( &rp.shadowColor, &rp.shadowColorView, &rp.shadowColorMemory );
}

/*
===============
the render passes and framebuffers: one colour image, read before and after
===============
*/
static VkRenderPass RP_CreatePass( VkFormat format, const char *name ) {
	VkAttachmentDescription att;
	VkAttachmentReference ref;
	VkSubpassDescription sub;
	VkSubpassDependency deps[2];
	VkRenderPassCreateInfo desc;
	VkRenderPass pass;

	Com_Memset( &att, 0, sizeof( att ) );
	att.format = format;
	att.samples = VK_SAMPLE_COUNT_1_BIT;
	att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;	// the view's part is drawn, the rest isn't read
	att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	att.initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	att.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	ref.attachment = 0;
	ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	Com_Memset( &sub, 0, sizeof( sub ) );
	sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	sub.colorAttachmentCount = 1;
	sub.pColorAttachments = &ref;

	deps[0] = vk.render_pass.subpass_deps.shader_to_color;
	deps[1] = vk.render_pass.subpass_deps.color_to_shader;

	Com_Memset( &desc, 0, sizeof( desc ) );
	desc.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	desc.attachmentCount = 1;
	desc.pAttachments = &att;
	desc.subpassCount = 1;
	desc.pSubpasses = &sub;
	desc.dependencyCount = ARRAY_LEN( deps );
	desc.pDependencies = deps;
	VK_CHECK( qvkCreateRenderPass( vk.device, &desc, NULL, &pass ) );
	VK_SET_OBJECT_NAME( pass, name, VK_DEBUG_REPORT_OBJECT_TYPE_RENDER_PASS_EXT );
	return pass;
}

// the shadow map's: the colour not kept, the depth cleared, kept and read after
static VkRenderPass RP_CreateShadowPass( void ) {
	VkAttachmentDescription att[2];
	VkAttachmentReference color, depth;
	VkSubpassDescription sub;
	VkSubpassDependency deps[2];
	VkRenderPassCreateInfo desc;
	VkRenderPass pass;

	Com_Memset( att, 0, sizeof( att ) );
	att[0].format = VK_FORMAT_R8_UNORM;
	att[0].samples = VK_SAMPLE_COUNT_1_BIT;
	att[0].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	att[0].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	att[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	att[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	att[1].format = rp.shadowFormat;
	att[1].samples = VK_SAMPLE_COUNT_1_BIT;
	att[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	att[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	att[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	att[1].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	color.attachment = 0;
	color.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	depth.attachment = 1;
	depth.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	Com_Memset( &sub, 0, sizeof( sub ) );
	sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	sub.colorAttachmentCount = 1;
	sub.pColorAttachments = &color;
	sub.pDepthStencilAttachment = &depth;

	Com_Memset( deps, 0, sizeof( deps ) );
	deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
	deps[0].dstSubpass = 0;
	deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	deps[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
	deps[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	deps[1].srcSubpass = 0;
	deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
	deps[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	deps[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

	Com_Memset( &desc, 0, sizeof( desc ) );
	desc.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	desc.attachmentCount = 2;
	desc.pAttachments = att;
	desc.subpassCount = 1;
	desc.pSubpasses = &sub;
	desc.dependencyCount = ARRAY_LEN( deps );
	desc.pDependencies = deps;
	VK_CHECK( qvkCreateRenderPass( vk.device, &desc, NULL, &pass ) );
	VK_SET_OBJECT_NAME( pass, "render pass - rpmod shadow map", VK_DEBUG_REPORT_OBJECT_TYPE_RENDER_PASS_EXT );
	return pass;
}

void vk_rpmod_create_render_passes( void ) {
	if ( !vk.fboActive || rp.postImage[0] == VK_NULL_HANDLE )
		return;
	if ( rp.shadowImage != VK_NULL_HANDLE )
		rp.shadowPass = RP_CreateShadowPass();
	rp.postPass = RP_CreatePass( vk.color_format, "render pass - rpmod post" );
	rp.distPass = RP_CreatePass( VK_FORMAT_R32_SFLOAT, "render pass - rpmod distance" );
	rp.aoPass = RP_CreatePass( VK_FORMAT_R8_UNORM, "render pass - rpmod ambient occlusion" );
}

void vk_rpmod_destroy_render_passes( void ) {
	VkRenderPass *passes[] = { &rp.postPass, &rp.distPass, &rp.aoPass, &rp.shadowPass };

	rp.ready = qfalse;
	for ( size_t i = 0; i < ARRAY_LEN( passes ); i++ ) {
		if ( *passes[i] != VK_NULL_HANDLE )
			qvkDestroyRenderPass( vk.device, *passes[i], NULL );
		*passes[i] = VK_NULL_HANDLE;
	}
}

static VkFramebuffer RP_CreateFramebuffer( VkRenderPass pass, VkImageView view, const char *name ) {
	VkFramebufferCreateInfo desc;
	VkFramebuffer fb;

	Com_Memset( &desc, 0, sizeof( desc ) );
	desc.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	desc.renderPass = pass;
	desc.attachmentCount = 1;
	desc.pAttachments = &view;
	desc.width = glConfig.vidWidth;
	desc.height = glConfig.vidHeight;
	desc.layers = 1;
	VK_CHECK( qvkCreateFramebuffer( vk.device, &desc, NULL, &fb ) );
	VK_SET_OBJECT_NAME( fb, name, VK_DEBUG_REPORT_OBJECT_TYPE_FRAMEBUFFER_EXT );
	return fb;
}

void vk_rpmod_create_framebuffers( void ) {
	if ( rp.postPass == VK_NULL_HANDLE )
		return;
	rp.postFb[0] = RP_CreateFramebuffer( rp.postPass, rp.postView[0], "framebuffer - rpmod post 0" );
	rp.postFb[1] = RP_CreateFramebuffer( rp.postPass, rp.postView[1], "framebuffer - rpmod post 1" );
	rp.distFb = RP_CreateFramebuffer( rp.distPass, rp.distView, "framebuffer - rpmod distance" );
	rp.aoFb[0] = RP_CreateFramebuffer( rp.aoPass, rp.aoView[0], "framebuffer - rpmod ambient occlusion 0" );
	rp.aoFb[1] = RP_CreateFramebuffer( rp.aoPass, rp.aoView[1], "framebuffer - rpmod ambient occlusion 1" );
	if ( rp.shadowPass != VK_NULL_HANDLE ) {
		VkImageView views[2] = { rp.shadowColorView, rp.shadowView };
		VkFramebufferCreateInfo desc;

		Com_Memset( &desc, 0, sizeof( desc ) );
		desc.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		desc.renderPass = rp.shadowPass;
		desc.attachmentCount = 2;
		desc.pAttachments = views;
		desc.width = desc.height = RP_SHADOW_SIZE;
		desc.layers = 1;
		VK_CHECK( qvkCreateFramebuffer( vk.device, &desc, NULL, &rp.shadowFb ) );
		VK_SET_OBJECT_NAME( rp.shadowFb, "framebuffer - rpmod shadow map", VK_DEBUG_REPORT_OBJECT_TYPE_FRAMEBUFFER_EXT );
	}
}

void vk_rpmod_destroy_framebuffers( void ) {
	VkFramebuffer *fbs[] = { &rp.postFb[0], &rp.postFb[1], &rp.distFb, &rp.aoFb[0], &rp.aoFb[1], &rp.shadowFb };

	rp.ready = qfalse;
	for ( size_t i = 0; i < ARRAY_LEN( fbs ); i++ ) {
		if ( *fbs[i] != VK_NULL_HANDLE )
			qvkDestroyFramebuffer( vk.device, *fbs[i], NULL );
		*fbs[i] = VK_NULL_HANDLE;
	}
}

/*
===============
the pipelines: a quad over the viewport, which is set as each pass draws
===============
*/
#define RP_BLEND_ADD	2	// RP_CreatePipeline's multiply: 1 multiplies, this adds

static VkPipeline RP_CreatePipeline( VkShaderModule fs, VkRenderPass pass, VkSampleCountFlagBits samples, int multiply, const char *name ) {
	VkPipelineShaderStageCreateInfo stages[2];
	VkPipelineVertexInputStateCreateInfo vertexInput;
	VkPipelineInputAssemblyStateCreateInfo inputAssembly;
	VkPipelineViewportStateCreateInfo viewportState;
	VkPipelineRasterizationStateCreateInfo raster;
	VkPipelineMultisampleStateCreateInfo multisample;
	VkPipelineColorBlendAttachmentState blendAttachment;
	VkPipelineColorBlendStateCreateInfo blend;
	VkPipelineDepthStencilStateCreateInfo depthStencil;
	VkDynamicState dynamicStates[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dynamic;
	VkGraphicsPipelineCreateInfo desc;
	VkPipeline pipeline;

	Com_Memset( stages, 0, sizeof( stages ) );
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vk.shaders.gamma_vs;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = fs;
	stages[1].pName = "main";

	Com_Memset( &vertexInput, 0, sizeof( vertexInput ) );
	vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

	Com_Memset( &inputAssembly, 0, sizeof( inputAssembly ) );
	inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

	Com_Memset( &viewportState, 0, sizeof( viewportState ) );
	viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1;
	viewportState.scissorCount = 1;

	Com_Memset( &raster, 0, sizeof( raster ) );
	raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode = VK_CULL_MODE_NONE;
	raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
	raster.lineWidth = 1.0f;

	Com_Memset( &multisample, 0, sizeof( multisample ) );
	multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = samples;
	multisample.minSampleShading = 1.0f;

	Com_Memset( &blendAttachment, 0, sizeof( blendAttachment ) );
	blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	if ( multiply == RP_BLEND_ADD ) {
		// what's there plus what's drawn, its alpha left alone
		blendAttachment.blendEnable = VK_TRUE;
		blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
		blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
		blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
		blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
		blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
	} else if ( multiply ) {
		// what's there times what's drawn
		blendAttachment.blendEnable = VK_TRUE;
		blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
		blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
		blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
		blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
		blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
	}
	Com_Memset( &blend, 0, sizeof( blend ) );
	blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	blend.attachmentCount = 1;
	blend.pAttachments = &blendAttachment;

	Com_Memset( &depthStencil, 0, sizeof( depthStencil ) );
	depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencil.depthCompareOp = VK_COMPARE_OP_ALWAYS;
	depthStencil.maxDepthBounds = 1.0f;

	Com_Memset( &dynamic, 0, sizeof( dynamic ) );
	dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamic.dynamicStateCount = ARRAY_LEN( dynamicStates );
	dynamic.pDynamicStates = dynamicStates;

	Com_Memset( &desc, 0, sizeof( desc ) );
	desc.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	desc.stageCount = 2;
	desc.pStages = stages;
	desc.pVertexInputState = &vertexInput;
	desc.pInputAssemblyState = &inputAssembly;
	desc.pViewportState = &viewportState;
	desc.pRasterizationState = &raster;
	desc.pMultisampleState = &multisample;
	desc.pDepthStencilState = &depthStencil;
	desc.pColorBlendState = &blend;
	desc.pDynamicState = &dynamic;
	desc.layout = rp.layout;
	desc.renderPass = pass;
	desc.subpass = 0;
	desc.basePipelineIndex = -1;
	VK_CHECK( qvkCreateGraphicsPipelines( vk.device, VK_NULL_HANDLE, 1, &desc, NULL, &pipeline ) );
	VK_SET_OBJECT_NAME( pipeline, name, VK_DEBUG_REPORT_OBJECT_TYPE_PIPELINE_EXT );
	return pipeline;
}

void vk_rpmod_destroy_pipelines( void ) {
	VkPipeline *pipes[] = { &rp.depthPipe, &rp.dofPipe, &rp.filterPipe, &rp.copyPipe, &rp.ssaoPipe, &rp.aoBlurPipe,
		&rp.aoApplyPipe, &rp.shaftsPipe[0], &rp.shaftsPipe[1], &rp.shaftsPipe[2], &rp.mblurPipe, &rp.gradePipe,
		&rp.fxaaPipe, &rp.casPipe, &rp.volumPipe, &rp.ssrPipe, &rp.shadowPipe };

	rp.ready = qfalse;
	for ( size_t i = 0; i < ARRAY_LEN( pipes ); i++ ) {
		if ( *pipes[i] != VK_NULL_HANDLE )
			qvkDestroyPipeline( vk.device, *pipes[i], NULL );
		*pipes[i] = VK_NULL_HANDLE;
	}
}

void vk_rpmod_create_pipelines( void ) {
	const VkSampleCountFlagBits one = VK_SAMPLE_COUNT_1_BIT;

	vk_rpmod_destroy_pipelines();
	if ( !vk.fboActive || rp.postFb[0] == VK_NULL_HANDLE || rp.layout == VK_NULL_HANDLE )
		return;

	if ( rp.usable ) {
		rp.depthPipe = RP_CreatePipeline( vk.msaaActive ? rp.depthMsFs : rp.depthFs, rp.distPass, one, qfalse, "rpmod depth to distance" );
		rp.ssaoPipe = RP_CreatePipeline( rp.ssaoFs, rp.aoPass, one, qfalse, "rpmod ambient occlusion" );
		rp.aoBlurPipe = RP_CreatePipeline( rp.aoBlurFs, rp.aoPass, one, qfalse, "rpmod ambient occlusion blur" );
		// into the view: drawn in the main render pass, as many samples as it has
		rp.aoApplyPipe = RP_CreatePipeline( rp.aoApplyFs, vk.render_pass.main.handle, (VkSampleCountFlagBits)vkSamples, qtrue, "rpmod ambient occlusion apply" );
		rp.volumPipe = RP_CreatePipeline( rp.volumFs, vk.render_pass.main.handle, (VkSampleCountFlagBits)vkSamples, RP_BLEND_ADD, "rpmod volumetric light" );
		if ( rp.shadowPass != VK_NULL_HANDLE )
			rp.shadowPipe = RP_CreatePipeline( rp.shadowFs, vk.render_pass.main.handle, (VkSampleCountFlagBits)vkSamples, qtrue, "rpmod real shadows" );
	}
	rp.dofPipe = RP_CreatePipeline( rp.dofFs, rp.postPass, one, qfalse, "rpmod depth of field" );
	rp.filterPipe = RP_CreatePipeline( rp.filterFs, rp.postPass, one, qfalse, "rpmod screen filter" );
	for ( int i = 0; i < 3; i++ )
		rp.shaftsPipe[i] = RP_CreatePipeline( rp.shaftsFs[i], rp.postPass, one, qfalse, "rpmod sun shafts" );
	rp.mblurPipe = RP_CreatePipeline( rp.mblurFs, rp.postPass, one, qfalse, "rpmod motion blur" );
	rp.gradePipe = RP_CreatePipeline( rp.gradeFs, rp.postPass, one, qfalse, "rpmod colour grading" );
	rp.fxaaPipe = RP_CreatePipeline( rp.fxaaFs, rp.postPass, one, qfalse, "rpmod fxaa" );
	rp.casPipe = RP_CreatePipeline( rp.casFs, rp.postPass, one, qfalse, "rpmod sharpen" );
	rp.ssrPipe = RP_CreatePipeline( rp.ssrFs, rp.postPass, one, qfalse, "rpmod reflections" );
	// back into the view
	rp.copyPipe = RP_CreatePipeline( rp.copyFs, vk.render_pass.main.handle, (VkSampleCountFlagBits)vkSamples, qfalse, "rpmod copy" );
	rp.ready = qtrue;
}

/*
===============
drawing
===============
*/

// the view's part of the screen, top down as the images are
static qboolean RP_ViewRect( VkRect2D *r ) {
	int x = backEnd.viewParms.viewportX;
	int y = glConfig.vidHeight - ( backEnd.viewParms.viewportY + backEnd.viewParms.viewportHeight );
	int w = backEnd.viewParms.viewportWidth, h = backEnd.viewParms.viewportHeight;

	if ( x < 0 ) { w += x; x = 0; }
	if ( y < 0 ) { h += y; y = 0; }
	if ( x + w > glConfig.vidWidth ) w = glConfig.vidWidth - x;
	if ( y + h > glConfig.vidHeight ) h = glConfig.vidHeight - y;
	if ( w <= 0 || h <= 0 )
		return qfalse;
	r->offset.x = x;
	r->offset.y = y;
	r->extent.width = (uint32_t)w;
	r->extent.height = (uint32_t)h;
	return qtrue;
}

static void RP_RectVec( const VkRect2D *r, float *v ) {
	v[0] = (float)r->offset.x;
	v[1] = (float)r->offset.y;
	v[2] = (float)( r->offset.x + r->extent.width );
	v[3] = (float)( r->offset.y + r->extent.height );
}

static void RP_TanHalf( float *v ) {
	v[0] = tanf( DEG2RAD( backEnd.viewParms.fovX ) * 0.5f );
	v[1] = tanf( DEG2RAD( backEnd.viewParms.fovY ) * 0.5f );
}

static void RP_SetViewport( const VkRect2D *r ) {
	VkViewport v;

	v.x = (float)r->offset.x;
	v.y = (float)r->offset.y;
	v.width = (float)r->extent.width;
	v.height = (float)r->extent.height;
	v.minDepth = 0.0f;
	v.maxDepth = 1.0f;
	qvkCmdSetViewport( vk.cmd->command_buffer, 0, 1, &v );
	qvkCmdSetScissor( vk.cmd->command_buffer, 0, 1, r );
}

static void RP_Quad( VkPipeline pipe, VkDescriptorSet set0, VkDescriptorSet set1, const void *push, uint32_t pushSize, const VkRect2D *r ) {
	VkDescriptorSet sets[2] = { set0, set1 };

	qvkCmdBindPipeline( vk.cmd->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe );
	qvkCmdBindDescriptorSets( vk.cmd->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, rp.layout, 0, set1 != VK_NULL_HANDLE ? 2 : 1, sets, 0, NULL );
	if ( push )
		qvkCmdPushConstants( vk.cmd->command_buffer, rp.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, pushSize, push );
	RP_SetViewport( r );
	qvkCmdDraw( vk.cmd->command_buffer, 4, 1, 0, 0 );
}

// one pass into an image of our own
static void RP_Pass( VkRenderPass pass, VkFramebuffer fb, VkPipeline pipe, VkDescriptorSet set0, VkDescriptorSet set1, const void *push, uint32_t pushSize, const VkRect2D *r ) {
	vk_begin_render_pass( pass, fb, qfalse, glConfig.vidWidth, glConfig.vidHeight );
	RP_Quad( pipe, set0, set1, push, pushSize, r );
	vk_end_render_pass();
}

// the depth buffer, between drawn into and read
static void RP_DepthBarrier( qboolean toRead ) {
	VkImageMemoryBarrier b;
	const VkPipelineStageFlags tests = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;

	Com_Memset( &b, 0, sizeof( b ) );
	b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	b.oldLayout = toRead ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
	b.newLayout = toRead ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	b.srcAccessMask = toRead ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT;
	b.dstAccessMask = toRead ? VK_ACCESS_SHADER_READ_BIT : ( VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT );
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = vk.depth_image;
	b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | ( glConfig.stencilBits > 0 ? VK_IMAGE_ASPECT_STENCIL_BIT : 0 );
	b.subresourceRange.levelCount = 1;
	b.subresourceRange.layerCount = 1;
	qvkCmdPipelineBarrier( vk.cmd->command_buffer,
		toRead ? tests : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		toRead ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : tests,
		0, 0, NULL, 0, NULL, 1, &b );
}

// back into the view's render pass, everything in it kept
static void RP_Resume( void ) {
	vk_begin_post_blend_render_pass( vk.render_pass.refraction.extract.handle, qfalse );
}

// and the state the renderer had: its pipeline, its image sets, its depth range and viewport
static void RP_Restore( void ) {
	uint32_t offsets[VK_DESC_UNIFORM_COUNT], offsetCount;

	vk.cmd->depth_range = DEPTH_RANGE_COUNT;
	if ( vk.cmd->last_pipeline != VK_NULL_HANDLE ) {
		qvkCmdBindPipeline( vk.cmd->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.cmd->last_pipeline );
		vk_update_mvp( NULL );
	}

	for ( int i = 0; i < VK_DESC_COUNT; i++ ) {
		if ( vk.cmd->descriptor_set.current[i] == VK_NULL_HANDLE )
			continue;
		if ( i == VK_DESC_UNIFORM ) {
			offsetCount = 0;
			offsets[offsetCount++] = vk.cmd->descriptor_set.offset[VK_DESC_UNIFORM_MAIN_BINDING];
			offsets[offsetCount++] = vk.cmd->descriptor_set.offset[VK_DESC_UNIFORM_CAMERA_BINDING];
			offsets[offsetCount++] = vk.cmd->descriptor_set.offset[VK_DESC_UNIFORM_ENTITY_BINDING];
			offsets[offsetCount++] = vk.cmd->descriptor_set.offset[VK_DESC_UNIFORM_BONES_BINDING];
			offsets[offsetCount++] = vk.cmd->descriptor_set.offset[VK_DESC_UNIFORM_FOGS_BINDING];
			offsets[offsetCount++] = vk.cmd->descriptor_set.offset[VK_DESC_UNIFORM_GLOBAL_BINDING];
			qvkCmdBindDescriptorSets( vk.cmd->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.pipeline_layout, i, 1, &vk.cmd->descriptor_set.current[i], offsetCount, offsets );
		} else {
			qvkCmdBindDescriptorSets( vk.cmd->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.pipeline_layout, i, 1, &vk.cmd->descriptor_set.current[i], 0, NULL );
		}
	}
}

// the world's main view in an image the passes can read; not the sky portal's scene, the main
// view after it covers that too
static qboolean RP_MainView( void ) {
	return (qboolean)( rp.ready && rp.setsAllocated && vk.fboActive && vk.renderPassIndex != RENDER_PASS_SCREENMAP
		&& backEnd.viewParms.portalView == PV_NONE && !( backEnd.refdef.rdflags & ( RDF_NOWORLDMODEL | RDF_SKYBOXPORTAL ) ) );
}

/*
===============
the sun, for sun shafts: where it is in the view and how much its beams show; qfalse when the
map's sky has no sun, or it's behind the camera or too far off the side of the view
===============
*/
static qboolean RP_Sun( const VkRect2D *r, float *pos, float *color ) {
	const float *axis0 = backEnd.viewParms.ori.axis[0], *axis1 = backEnd.viewParms.ori.axis[1], *axis2 = backEnd.viewParms.ori.axis[2];
	const float m = MAX( tr.sunLight[0], MAX( tr.sunLight[1], tr.sunLight[2] ) );
	float tanHalf[2], z, nx, ny, fade;

	if ( !r_sunShafts->integer || r_sunShaftsIntensity->value <= 0.0f || m <= 0.0f )
		return qfalse;

	// in the camera's space as the shaders have it: x right, y down, z forward
	z = DotProduct( tr.sunDirection, axis0 );
	if ( z <= 0.05f )
		return qfalse;
	RP_TanHalf( tanHalf );
	nx = -DotProduct( tr.sunDirection, axis1 ) / ( z * tanHalf[0] );
	ny = -DotProduct( tr.sunDirection, axis2 ) / ( z * tanHalf[1] );
	fade = Com_Clamp( 0.0f, 1.0f, ( z - 0.05f ) * 4.0f ) * Com_Clamp( 0.0f, 1.0f, 2.5f - MAX( fabsf( nx ), fabsf( ny ) ) );
	if ( fade <= 0.0f )
		return qfalse;

	pos[0] = r->offset.x + ( nx * 0.5f + 0.5f ) * r->extent.width;
	pos[1] = r->offset.y + ( ny * 0.5f + 0.5f ) * r->extent.height;
	for ( int i = 0; i < 3; i++ )
		color[i] = tr.sunLight[i] / m * r_sunShaftsIntensity->value * fade;
	color[3] = 0.0f;
	return qtrue;
}

/*
===============
volumetric light: the dynamic lights that glow most in this view, nearest and biggest first, in the
camera's space as the shaders have it (x right, y down, z forward); how many
===============
*/
#define RP_VOLUM_MAX	12		// three a pass

static int RP_VolumLights( vec4_t *pos, vec4_t *color ) {
	const float *org = backEnd.viewParms.ori.origin;
	const float *axis0 = backEnd.viewParms.ori.axis[0], *axis1 = backEnd.viewParms.ori.axis[1], *axis2 = backEnd.viewParms.ori.axis[2];
	float score[RP_VOLUM_MAX];
	int n = 0;

	if ( !r_volumetric->integer || r_volumetricIntensity->value <= 0.0f || rp.volumPipe == VK_NULL_HANDLE || !backEnd.refdef.dlights )
		return 0;
	for ( int i = 0; i < backEnd.refdef.num_dlights; i++ ) {
		const dlight_t *dl = &backEnd.refdef.dlights[i];
		vec3_t at, d;
		float s, bright;
		int k;

		if ( dl->radius < 8.0f )
			continue;
		bright = MAX( dl->color[0], MAX( dl->color[1], dl->color[2] ) );
		if ( bright <= 0.0f )
			continue;
		// a light along a line (a blaster bolt's trail): its middle
		if ( dl->linear )
			VectorAdd( dl->origin, dl->origin2, at ), VectorScale( at, 0.5f, at );
		else
			VectorCopy( dl->origin, at );
		VectorSubtract( at, org, d );
		// all of it behind the camera
		if ( DotProduct( d, axis0 ) < -dl->radius )
			continue;
		s = bright * dl->radius / ( VectorLength( d ) + dl->radius );
		// in order, the weakest dropped once there are enough
		for ( k = n; k > 0 && score[k - 1] < s; k-- ) {
			if ( k < RP_VOLUM_MAX ) {
				score[k] = score[k - 1];
				Com_Memcpy( pos[k], pos[k - 1], sizeof( vec4_t ) );
				Com_Memcpy( color[k], color[k - 1], sizeof( vec4_t ) );
			}
		}
		if ( k >= RP_VOLUM_MAX )
			continue;
		score[k] = s;
		pos[k][0] = -DotProduct( d, axis1 );
		pos[k][1] = -DotProduct( d, axis2 );
		pos[k][2] = DotProduct( d, axis0 );
		pos[k][3] = dl->radius;
		VectorCopy( dl->color, color[k] );
		color[k][3] = 0.0f;
		if ( n < RP_VOLUM_MAX )
			n++;
	}
	return n;
}

// what needs the view's distances
static qboolean RP_NeedDistance( const VkRect2D *r ) {
	float pos[2], color[4];

	if ( !rp.usable || rp.depthPipe == VK_NULL_HANDLE )
		return qfalse;
	return (qboolean)( backEnd.refdef.dofBlur > 0.0f || r_ssao->integer || r_softParticles->integer
		|| r_motionBlur->value > 0.0f || RP_Sun( r, pos, color )
		|| ( r_volumetric->integer && backEnd.refdef.num_dlights > 0 && rp.volumPipe != VK_NULL_HANDLE )
		|| ( r_ssr->integer && r_ssrStrength->value > 0.0f )
		|| ( r_shadowMaps->integer && r_shadowStrength->value > 0.0f && rp.shadowPipe != VK_NULL_HANDLE ) );
}

/*
===============
vk_rpmod_split

Where the view's surfaces split into the solid ones and the see-through ones drawn over them, if
anything needs the distances in between; -1 if not
===============
*/
int vk_rpmod_split( const drawSurf_t *drawSurfs, int numDrawSurfs ) {
	VkRect2D r;
	int i;

	rp.haveDistance = rp.softOn = qfalse;
	if ( !RP_MainView() || !RP_ViewRect( &r ) || !RP_NeedDistance( &r ) )
		return -1;

	for ( i = 0; i < numDrawSurfs; i++ ) {
		int entityNum, fogNum, dlighted;
		shader_t *shader;

		R_DecomposeSort( drawSurfs[i].sort, &entityNum, &shader, &fogNum, &dlighted );
		if ( shader->sort >= SS_BANNER )
			break;
	}
	return i;
}

/*
===============
vk_rpmod_after_opaque

Between the solid surfaces and the see-through ones: the view's distances, ambient occlusion from
them multiplied into the view, and soft particles on from here
===============
*/
/*
===============
real shadows: the light's camera, looking along the light at the place round you, square and as
deep as RP_SHADOW_RANGE (orthographic); and the matrix from the view's camera space to that light's
===============
*/
void RB_RenderDrawSurfList( drawSurf_t *drawSurfs, int numDrawSurfs );	// tr_backend.cpp

static const float rpFlip[16] = {	// our coordinate system (looking down X) to OpenGL's (down -Z)
	0, 0, -1, 0,
	-1, 0, 0, 0,
	0, 1, 0, 0,
	0, 0, 0, 1
};

static void RP_ShadowLight( viewParms_t *lp, float *toLight, float *bias ) {
	const float R = r_shadowDistance->value;
	const float texel = 2.0f * R / RP_SHADOW_SIZE;
	const float *org = backEnd.viewParms.ori.origin, *axis0 = backEnd.viewParms.ori.axis[0];
	const float *axis1 = backEnd.viewParms.ori.axis[1], *axis2 = backEnd.viewParms.ori.axis[2];
	vec3_t L, f, up, left, hint, fwd, C;
	float viewer[16], proj[16], lightMvp[16], A[16], a, b;

	// toward the light: the map's sun, or nearly straight down
	if ( r_shadowSun->integer && tr.sunDirection[2] > 0.25f )
		VectorCopy( tr.sunDirection, L );
	else
		VectorSet( L, 0.4f, 0.25f, 1.0f );
	VectorNormalize( L );
	VectorScale( L, -1.0f, f );
	if ( fabsf( f[2] ) > 0.99f )
		VectorSet( hint, 1.0f, 0.0f, 0.0f );
	else
		VectorSet( hint, 0.0f, 0.0f, 1.0f );
	VectorMA( hint, -DotProduct( hint, f ), f, up );
	VectorNormalize( up );
	CrossProduct( up, f, left );

	// round you, more ahead than behind; whole texels across the light, so it doesn't shimmer as you move
	VectorSet( fwd, axis0[0], axis0[1], 0.0f );
	if ( VectorNormalize( fwd ) < 0.01f )
		VectorClear( fwd );
	VectorMA( org, R * 0.4f, fwd, C );
	a = DotProduct( C, left );
	b = DotProduct( C, up );
	VectorMA( C, floorf( a / texel ) * texel - a, left, C );
	VectorMA( C, floorf( b / texel ) * texel - b, up, C );

	*lp = backEnd.viewParms;
	VectorMA( C, RP_SHADOW_RANGE * 0.5f, L, lp->ori.origin );
	VectorCopy( f, lp->ori.axis[0] );
	VectorCopy( left, lp->ori.axis[1] );
	VectorCopy( up, lp->ori.axis[2] );
	lp->portalView = PV_NONE;
	lp->viewportX = lp->viewportY = 0;
	lp->viewportWidth = lp->viewportHeight = RP_SHADOW_SIZE;
	lp->zNear = 1.0f;
	lp->zFar = RP_SHADOW_RANGE;

	// its world matrix, as R_RotateForViewer makes the camera's
	Com_Memset( &lp->world, 0, sizeof( lp->world ) );
	lp->world.axis[0][0] = lp->world.axis[1][1] = lp->world.axis[2][2] = 1.0f;
	VectorCopy( lp->ori.origin, lp->world.viewOrigin );
	for ( int i = 0; i < 3; i++ ) {
		viewer[i * 4 + 0] = lp->ori.axis[0][i];
		viewer[i * 4 + 1] = lp->ori.axis[1][i];
		viewer[i * 4 + 2] = lp->ori.axis[2][i];
		viewer[i * 4 + 3] = 0.0f;
	}
	viewer[12] = -DotProduct( lp->ori.origin, lp->ori.axis[0] );
	viewer[13] = -DotProduct( lp->ori.origin, lp->ori.axis[1] );
	viewer[14] = -DotProduct( lp->ori.origin, lp->ori.axis[2] );
	viewer[15] = 1.0f;
	myGlMultMatrix( viewer, rpFlip, lp->world.modelViewMatrix );
	Matrix16Identity( lp->world.modelMatrix );

	// orthographic: across -R..R, depth 0 at the light to 1 RP_SHADOW_RANGE along it
	Com_Memset( proj, 0, sizeof( proj ) );
	proj[0] = 1.0f / R;
	proj[5] = 1.0f / R;
	proj[10] = -1.0f / RP_SHADOW_RANGE;
	proj[15] = 1.0f;
	Com_Memcpy( lp->projectionMatrix, proj, sizeof( proj ) );

	// the view's camera space (x right, y down, z forward) to the world, then to the light's, its y
	// turned as the renderer turns it for Vulkan
	Com_Memset( A, 0, sizeof( A ) );
	for ( int i = 0; i < 3; i++ ) {
		A[0 + i] = -axis1[i];
		A[4 + i] = -axis2[i];
		A[8 + i] = axis0[i];
		A[12 + i] = org[i];
	}
	A[15] = 1.0f;
	proj[5] = -proj[5];
	myGlMultMatrix( lp->world.modelViewMatrix, proj, lightMvp );
	myGlMultMatrix( A, lightMvp, toLight );

	// against a surface shadowing itself: a couple of texels and a bit, along the light
	*bias = ( 2.0f * texel + 2.0f ) / RP_SHADOW_RANGE;
}

// the models that cast shadows: solid ones, not the view weapon or what says it casts none
static int RP_Casters( drawSurf_t *drawSurfs, int numDrawSurfs, drawSurf_t *out ) {
	int n = 0;

	for ( int i = 0; i < numDrawSurfs && n < RP_MAX_CASTERS; i++ ) {
		int entityNum, fogNum, dlighted;
		shader_t *shader;
		const trRefEntity_t *ent;

		R_DecomposeSort( drawSurfs[i].sort, &entityNum, &shader, &fogNum, &dlighted );
		if ( entityNum == REFENTITYNUM_WORLD || shader->sort > SS_OPAQUE || shader->isSky || shader->useDistortion )
			continue;
		ent = &backEnd.refdef.entities[entityNum];
		if ( ent->e.reType != RT_MODEL || ( ent->e.renderfx & ( RF_FIRST_PERSON | RF_DEPTHHACK | RF_NOSHADOW | RF_DISTORTION ) ) )
			continue;
		out[n++] = drawSurfs[i];
	}
	return n;
}

// the casters drawn from the light into the shadow map by the renderer's own drawing, as it draws them
// for the view, only with the light's camera
static qboolean RP_ShadowMap( drawSurf_t *drawSurfs, int numDrawSurfs, float *toLight, float *bias ) {
	static drawSurf_t casters[RP_MAX_CASTERS];
	static viewParms_t saved;
	viewParms_t light;
	orientationr_t savedOri;
	const int n = RP_Casters( drawSurfs, numDrawSurfs, casters );

	if ( !n )
		return qfalse;
	RP_ShadowLight( &light, toLight, bias );

	saved = backEnd.viewParms;
	savedOri = backEnd.ori;
	backEnd.viewParms = light;
	vk.renderPassIndex = RENDER_PASS_SHADOW;
	vk.cmd->last_pipeline = VK_NULL_HANDLE;
	vk_begin_render_pass( rp.shadowPass, rp.shadowFb, qtrue, RP_SHADOW_SIZE, RP_SHADOW_SIZE );
	Com_Memset( &vk.cmd->scissor_rect, 0, sizeof( vk.cmd->scissor_rect ) );
	RB_RenderDrawSurfList( casters, n );
	vk_end_render_pass();

	backEnd.viewParms = saved;
	backEnd.ori = savedOri;
	vk.renderPassIndex = RENDER_PASS_MAIN;
	vk.cmd->last_pipeline = VK_NULL_HANDLE;
	vk.cmd->depth_range = DEPTH_RANGE_COUNT;
	Com_Memset( &vk.cmd->scissor_rect, 0, sizeof( vk.cmd->scissor_rect ) );
	Com_Memcpy( vk_world.modelview_transform, backEnd.viewParms.world.modelViewMatrix, 64 );
	return qtrue;
}

void vk_rpmod_after_opaque( drawSurf_t *drawSurfs, int numDrawSurfs ) {
	const float zNear = backEnd.viewParms.zNear, zFar = backEnd.viewParms.zFar;
	const qboolean ssao = (qboolean)( r_ssao->integer && r_ssaoIntensity->value > 0.0f && rp.ssaoPipe != VK_NULL_HANDLE );
	float push[4], toLight[16], shadowBias = 0.0f;
	qboolean shadows = qfalse;
	VkRect2D r;

	if ( zFar <= zNear || !RP_ViewRect( &r ) )
		return;

	push[0] = zNear * zFar;
	push[1] = zFar;
	push[2] = zFar - zNear;
	push[3] = 0.0f;

	vk_end_render_pass();
	if ( r_shadowMaps->integer && r_shadowStrength->value > 0.0f && rp.shadowPipe != VK_NULL_HANDLE && rp.shadowFb != VK_NULL_HANDLE )
		shadows = RP_ShadowMap( drawSurfs, numDrawSurfs, toLight, &shadowBias );
	RP_DepthBarrier( qtrue );
	RP_Pass( rp.distPass, rp.distFb, rp.depthPipe, rp.depthSet, VK_NULL_HANDLE, push, sizeof( push ), &r );
	RP_DepthBarrier( qfalse );

	if ( ssao ) {
		const float radius = r_ssaoRadius->value;
		struct {
			float rect[4];
			float a[4];
			float b[4];
		} p;
		struct {
			float rect[4];
			float dir[4];
		} blur;

		RP_RectVec( &r, p.rect );
		RP_TanHalf( p.a );
		p.a[2] = radius;
		p.a[3] = r_ssaoIntensity->value;
		p.b[0] = zFar;
		p.b[1] = 0.0f;
		p.b[2] = (float)r.extent.height;
		p.b[3] = 0.0f;
		RP_Pass( rp.aoPass, rp.aoFb[0], rp.ssaoPipe, rp.distSet, VK_NULL_HANDLE, &p, sizeof( p ), &r );

		Com_Memset( &blur, 0, sizeof( blur ) );
		RP_RectVec( &r, blur.rect );
		blur.dir[0] = 1.0f;
		RP_Pass( rp.aoPass, rp.aoFb[1], rp.aoBlurPipe, rp.aoSet[0], rp.distSet, &blur, sizeof( blur ), &r );
		blur.dir[0] = 0.0f;
		blur.dir[1] = 1.0f;
		RP_Pass( rp.aoPass, rp.aoFb[0], rp.aoBlurPipe, rp.aoSet[1], rp.distSet, &blur, sizeof( blur ), &r );
	}

	RP_Resume();
	vk.renderPassIndex = RENDER_PASS_MAIN;	// the see-through surfaces' pipelines as ever
	if ( ssao )
		RP_Quad( rp.aoApplyPipe, rp.aoSet[0], VK_NULL_HANDLE, NULL, 0, &r );
	// what the shadow map hides from the light, darkened
	if ( shadows ) {
		struct {
			float rect[4];
			float a[4];
			float b[4];
			float m[16];
		} p;

		RP_RectVec( &r, p.rect );
		RP_TanHalf( p.a );
		p.a[2] = r_shadowStrength->value;
		p.a[3] = zFar;
		p.b[0] = 1.0f / RP_SHADOW_SIZE;
		p.b[1] = r_shadowSoftness->value;
		p.b[2] = shadowBias;
		p.b[3] = 0.0f;
		Com_Memcpy( p.m, toLight, sizeof( p.m ) );
		RP_Quad( rp.shadowPipe, rp.distSet, rp.shadowSet, &p, sizeof( p ), &r );
	}
	// the lights' glow in the air, added, three lights a quad
	{
		vec4_t lpos[RP_VOLUM_MAX], lcolor[RP_VOLUM_MAX];
		const int n = RP_VolumLights( lpos, lcolor );
		float strength = r_volumetricIntensity->value;
		struct {
			float rect[4];
			float a[4];
			float light[3][4];
			float color[3][4];
		} p;

		// thicker air on a map with fog all over
		if ( tr.world && tr.world->globalFog != -1 )
			strength *= r_volumetricFog->value;
		for ( int first = 0; first < n; first += 3 ) {
			const int count = MIN( 3, n - first );

			Com_Memset( &p, 0, sizeof( p ) );
			RP_RectVec( &r, p.rect );
			RP_TanHalf( p.a );
			p.a[2] = strength;
			p.a[3] = (float)count;
			for ( int k = 0; k < count; k++ ) {
				Com_Memcpy( p.light[k], lpos[first + k], sizeof( vec4_t ) );
				Com_Memcpy( p.color[k], lcolor[first + k], sizeof( vec4_t ) );
			}
			RP_Quad( rp.volumPipe, rp.distSet, VK_NULL_HANDLE, &p, sizeof( p ), &r );
		}
	}
	RP_Restore();

	rp.haveDistance = qtrue;
	rp.softOn = (qboolean)( r_softParticles->integer != 0 );
}

/*
===============
soft particles: the renderer's own pipelines for an effect's single texture stage swapped for ones
with a shader of ours, which reads the distances where a second texture would be
===============
*/
qboolean vk_rpmod_soft_active( void ) {
	return (qboolean)( rp.softOn && vk.renderPassIndex == RENDER_PASS_MAIN && !backEnd.isGlowPass && !backEnd.projection2D );
}

VkDescriptorSet vk_rpmod_soft_set( void ) {
	return rp.distSet;
}

// the pipeline with soft particles in place of the given one, or that one if they don't suit it
uint32_t vk_rpmod_soft_pipeline( uint32_t pipeline ) {
	Vk_Pipeline_Def def;
	uint32_t src, dst;
	int mode;

	vk_get_pipeline_def( pipeline, &def );
	if ( def.rpmod_soft || def.surface_sprite_flags || def.shadow_phase != SHADOW_DISABLED
		|| def.primitives != TRIANGLE_LIST
#ifdef USE_VBO
		|| def.vbo_ghoul2 || def.vbo_mdv
#endif
		|| ( def.state_bits & GLS_DEPTHMASK_TRUE ) )
		return pipeline;

	switch ( def.shader_type ) {
		case TYPE_SINGLE_TEXTURE:
		case TYPE_SINGLE_TEXTURE_ENV:
		case TYPE_SINGLE_TEXTURE_IDENTITY:
		case TYPE_SINGLE_TEXTURE_IDENTITY_ENV:
		case TYPE_SINGLE_TEXTURE_FIXED_COLOR:
		case TYPE_SINGLE_TEXTURE_FIXED_COLOR_ENV:
			break;
		default:
			return pipeline;
	}

	// how it blends decides how it fades
	src = def.state_bits & GLS_SRCBLEND_BITS;
	dst = def.state_bits & GLS_DSTBLEND_BITS;
	if ( ( src == GLS_SRCBLEND_ONE && ( dst == GLS_DSTBLEND_ONE || dst == GLS_DSTBLEND_ONE_MINUS_SRC_COLOR || dst == GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA ) )
		|| ( src == GLS_SRCBLEND_ZERO && dst == GLS_DSTBLEND_ONE_MINUS_SRC_COLOR ) )
		mode = 1;	// adds, or takes away: scaled to nothing
	else if ( ( src == GLS_SRCBLEND_DST_COLOR && dst == GLS_DSTBLEND_ZERO ) || ( src == GLS_SRCBLEND_ZERO && dst == GLS_DSTBLEND_SRC_COLOR ) )
		mode = 2;	// multiplies: taken to white
	else if ( src == GLS_SRCBLEND_SRC_ALPHA && ( dst == GLS_DSTBLEND_ONE || dst == GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA ) )
		mode = 3;	// by its alpha: that scaled to nothing
	else
		return pipeline;

	// how far it fades over goes in too, so a new one is a new pipeline
	def.rpmod_soft = mode | ( Com_Clampi( 1, 256, r_softParticlesDepth->integer ) << 2 );
	return vk_find_pipeline_ext( 0, &def, qfalse );
}

// for vk_create_pipeline: our fragment shader for the renderer's single texture one; the fogged one
// is the next, as the renderer's own are
VkShaderModule *vk_rpmod_soft_module( int shaderType ) {
	switch ( shaderType ) {
		case TYPE_SINGLE_TEXTURE:
		case TYPE_SINGLE_TEXTURE_ENV:
			return &rp.softFs[0];
		case TYPE_SINGLE_TEXTURE_IDENTITY:
		case TYPE_SINGLE_TEXTURE_IDENTITY_ENV:
			return &rp.softFs[2];
		case TYPE_SINGLE_TEXTURE_FIXED_COLOR:
		case TYPE_SINGLE_TEXTURE_FIXED_COLOR_ENV:
			return &rp.softFs[4];
		default:
			return NULL;
	}
}

// and its constants: the alpha test, fixed colour and fog as the renderer's, how it fades
const VkSpecializationInfo *vk_rpmod_soft_spec( int soft, int alphaTestFunc, float alphaTestValue, float identityColor, float identityAlpha, int hwFog, int acff ) {
	static struct {
		int32_t		alphaTestFunc;
		float		alphaTestValue;
		float		identityColor;
		float		identityAlpha;
		float		scale;
		int32_t		mode;
		int32_t		hwFog;
		int32_t		acff;
	} data;
	static VkSpecializationMapEntry entries[8];
	static VkSpecializationInfo info;
	const uint32_t ids[8] = { 0, 1, 9, 10, 20, 21, 5, 11 };

	data.alphaTestFunc = alphaTestFunc;
	data.alphaTestValue = alphaTestValue;
	data.identityColor = identityColor;
	data.identityAlpha = identityAlpha;
	data.scale = 1.0f / (float)MAX( 1, soft >> 2 );
	data.mode = soft & 3;
	data.hwFog = hwFog;
	data.acff = acff;

	for ( int i = 0; i < 8; i++ ) {
		entries[i].constantID = ids[i];
		entries[i].offset = i * 4;
		entries[i].size = 4;
	}
	info.mapEntryCount = ARRAY_LEN( entries );
	info.pMapEntries = entries;
	info.dataSize = sizeof( data );
	info.pData = &data;
	return &info;
}

/*
===============
the main view's camera, kept for the next frame's motion blur
===============
*/
static void RP_KeepCamera( const VkRect2D *r ) {
	VectorCopy( backEnd.viewParms.ori.origin, rp.prevOrigin );
	AxisCopy( backEnd.viewParms.ori.axis, rp.prevAxis );
	rp.prevTime = backEnd.refdef.time;
	rp.prevRect = *r;
	rp.prevValid = qtrue;
}

// this camera's space (x right, y down, z forward) to the one before's: rows, then where this
// camera is in that one's; qfalse when there's nothing to blur from
static qboolean RP_MotionBlur( const VkRect2D *r, float *m, float *scale ) {
	vec3_t cur[3], prev[3], d;
	int dt;

	if ( r_motionBlur->value <= 0.0f || !rp.prevValid || rp.mblurPipe == VK_NULL_HANDLE )
		return qfalse;
	dt = backEnd.refdef.time - rp.prevTime;
	if ( dt <= 0 || dt > 200 || memcmp( &rp.prevRect, r, sizeof( *r ) ) )
		return qfalse;

	// a cut or a teleport: not a move
	VectorSubtract( backEnd.viewParms.ori.origin, rp.prevOrigin, d );
	if ( VectorLength( d ) > 256.0f || DotProduct( backEnd.viewParms.ori.axis[0], rp.prevAxis[0] ) < 0.5f )
		return qfalse;

	for ( int i = 0; i < 3; i++ ) {
		cur[0][i] = -backEnd.viewParms.ori.axis[1][i];
		cur[1][i] = -backEnd.viewParms.ori.axis[2][i];
		cur[2][i] = backEnd.viewParms.ori.axis[0][i];
		prev[0][i] = -rp.prevAxis[1][i];
		prev[1][i] = -rp.prevAxis[2][i];
		prev[2][i] = rp.prevAxis[0][i];
	}
	for ( int row = 0; row < 3; row++ ) {
		for ( int col = 0; col < 3; col++ )
			m[row * 4 + col] = DotProduct( prev[row], cur[col] );
		m[row * 4 + 3] = 0.0f;
		m[12 + row] = DotProduct( prev[row], d );
	}
	m[15] = 0.0f;

	// as long a smear as a frame of 60 a second would leave, whatever the frame rate
	*scale = r_motionBlur->value * Com_Clamp( 0.25f, 4.0f, ( 1000.0f / 60.0f ) / dt );
	return qtrue;
}

// the colour grading to use, the map's own or the settings'; qfalse if it would change nothing
static qboolean RP_Grade( float *a, float *tint ) {
	float exposure, saturation, contrast, warmth;
	int tonemap;

	if ( mapGrade.valid && r_mapGrades->integer ) {
		exposure = mapGrade.exposure;
		saturation = mapGrade.saturation;
		contrast = mapGrade.contrast;
		warmth = mapGrade.warmth;
		tonemap = mapGrade.tonemap;
	} else {
		exposure = r_exposure->value;
		saturation = r_saturation->value;
		contrast = r_contrast->value;
		warmth = r_warmth->value;
		tonemap = r_tonemap->integer;
	}
	if ( !tonemap && exposure == 1.0f && saturation == 1.0f && contrast == 1.0f && warmth == 0.0f )
		return qfalse;

	a[0] = exposure;
	a[1] = (float)tonemap;	// 0 off, 1 soft, 2 film
	a[2] = saturation;
	a[3] = contrast;
	tint[0] = 1.0f + 0.15f * warmth;
	tint[1] = 1.0f + 0.03f * warmth;
	tint[2] = 1.0f - 0.15f * warmth;
	tint[3] = 1.0f;
	return qtrue;
}

/*
===============
vk_rpmod_post

After dynamic glow: the passes over the view, each from the last one's image into the other
===============
*/
void vk_rpmod_post( void ) {
	const qboolean dist = rp.haveDistance;
	const qboolean dof = (qboolean)( dist && backEnd.refdef.dofBlur > 0.0f );
	const qboolean filter = (qboolean)( backEnd.refdef.filterOn && backEnd.refdef.filter.strength > 0.0f );
	const qboolean fxaa = (qboolean)( r_fxaa->integer != 0 );
	const qboolean sharpen = (qboolean)( r_sharpen->value > 0.0f );
	const qboolean ssr = (qboolean)( dist && r_ssr->integer && r_ssrStrength->value > 0.0f && rp.ssrPipe != VK_NULL_HANDLE );
	float sunPos[2], sunColor[4], mblur[16], mblurScale = 0.0f, grade[4], tint[4];
	qboolean shafts, motion, graded;
	VkDescriptorSet src = rp.sceneSet;
	int next = 0;
	VkRect2D r;

	rp.haveDistance = rp.softOn = qfalse;
	if ( !RP_MainView() || !RP_ViewRect( &r ) )
		return;

	shafts = (qboolean)( dist && RP_Sun( &r, sunPos, sunColor ) );
	motion = (qboolean)( dist && RP_MotionBlur( &r, mblur, &mblurScale ) );
	graded = RP_Grade( grade, tint );
	RP_KeepCamera( &r );
	if ( !shafts && !ssr && !motion && !dof && !graded && !filter && !fxaa && !sharpen )
		return;

	vk_end_render_pass();

#define RP_NEXT( pipe, set1, push ) \
	RP_Pass( rp.postPass, rp.postFb[next], pipe, src, set1, &push, sizeof( push ), &r ); \
	src = rp.postSet[next]; \
	next ^= 1;

	if ( shafts ) {
		// the sky's bright parts, smeared toward the sun twice, added to the view: always first,
		// so both images are free for the beams
		struct {
			float rect[4];
			float sun[4];
			float color[4];
		} p;

		RP_RectVec( &r, p.rect );
		p.sun[0] = sunPos[0];
		p.sun[1] = sunPos[1];
		p.sun[2] = backEnd.viewParms.zFar;
		p.sun[3] = 0.6f;
		Com_Memcpy( p.color, sunColor, sizeof( p.color ) );
		RP_Pass( rp.postPass, rp.postFb[0], rp.shaftsPipe[0], rp.sceneSet, rp.distSet, &p, sizeof( p ), &r );
		RP_Pass( rp.postPass, rp.postFb[1], rp.shaftsPipe[1], rp.postSet[0], VK_NULL_HANDLE, &p, sizeof( p ), &r );
		p.sun[3] = 0.25f;
		RP_Pass( rp.postPass, rp.postFb[0], rp.shaftsPipe[1], rp.postSet[1], VK_NULL_HANDLE, &p, sizeof( p ), &r );
		RP_Pass( rp.postPass, rp.postFb[1], rp.shaftsPipe[2], rp.sceneSet, rp.postSet[0], &p, sizeof( p ), &r );
		src = rp.postSet[1];
		next = 0;
	}

	if ( ssr ) {
		// floors mirror the view, before anything blurs or grades it
		const float *axis0 = backEnd.viewParms.ori.axis[0], *axis1 = backEnd.viewParms.ori.axis[1], *axis2 = backEnd.viewParms.ori.axis[2];
		struct {
			float rect[4];
			float a[4];
			float up[4];
		} p;

		RP_RectVec( &r, p.rect );
		RP_TanHalf( p.a );
		p.a[2] = r_ssrStrength->value;
		p.a[3] = backEnd.viewParms.zFar;
		// the world's up in the camera's space: x right, y down, z forward
		p.up[0] = -axis1[2];
		p.up[1] = -axis2[2];
		p.up[2] = axis0[2];
		p.up[3] = 0.0f;
		RP_NEXT( rp.ssrPipe, rp.distSet, p );
	}

	if ( motion ) {
		struct {
			float m[16];
			float a[4];
			float rect[4];
		} p;

		Com_Memcpy( p.m, mblur, sizeof( p.m ) );
		RP_TanHalf( p.a );
		p.a[2] = mblurScale;
		p.a[3] = r.extent.height * 0.06f;
		RP_RectVec( &r, p.rect );
		RP_NEXT( rp.mblurPipe, rp.distSet, p );
	}

	if ( dof ) {
		const float radius = backEnd.refdef.dofBlur * glConfig.vidHeight / 480.0f;
		struct {
			float a[4];
			float rect[4];
		} p;

		p.a[0] = backEnd.refdef.dofFocus;
		p.a[1] = 1.0f / backEnd.refdef.dofRange;
		p.a[2] = radius;
		p.a[3] = r_dofBokeh->value;
		RP_RectVec( &r, p.rect );
		RP_NEXT( rp.dofPipe, rp.distSet, p );
		// as rd-vanilla: a second time, half as far out, to fill in between the first's taps
		if ( p.a[3] <= 0.0f ) {
			p.a[2] = radius * 0.5f;
			RP_NEXT( rp.dofPipe, rp.distSet, p );
		}
	}

	if ( graded ) {
		struct {
			float a[4];
			float tint[4];
		} p;

		Com_Memcpy( p.a, grade, sizeof( p.a ) );
		Com_Memcpy( p.tint, tint, sizeof( p.tint ) );
		RP_NEXT( rp.gradePipe, VK_NULL_HANDLE, p );
	}

	if ( filter ) {
		const screenFilter_t *f = &backEnd.refdef.filter;
		const float w = (float)r.extent.width, h = (float)r.extent.height;
		const float px = glConfig.vidHeight / 480.0f;
		const float grainPx = px > 1.0f ? px : 1.0f;
		struct {
			float mr[4], mg[4], mb[4];
			float p[4], q[4], view[4], nz[4];
			uint32_t ramp[4];
		} p;

		Com_Memset( &p, 0, sizeof( p ) );
		for ( int i = 0; i < 4; i++ ) {
			p.mr[i] = f->matrix[0][i];
			p.mg[i] = f->matrix[1][i];
			p.mb[i] = f->matrix[2][i];
		}
		p.p[0] = f->ramp;
		p.p[1] = f->grain;
		p.p[2] = f->vignette * 0.5f;
		p.p[3] = f->strength;
		p.q[0] = f->scanlines;
		p.q[1] = 2.0f * M_PI * f->scanlineCount / h;
		p.q[2] = f->aberration * px;
		p.q[3] = f->rampGain > 0.0f ? f->rampGain : 1.0f;
		p.view[0] = r.offset.x + w * 0.5f;
		p.view[1] = r.offset.y + h * 0.5f;
		p.view[2] = 2.0f / w;
		p.view[3] = 2.0f / h;
		p.nz[0] = ( rand() & 1023 ) / 1024.0f;
		p.nz[1] = ( rand() & 1023 ) / 1024.0f;
		p.nz[2] = 1.0f / grainPx;
		// the ramp's five colours, three bytes each
		for ( int k = 0; k < 5; k++ ) {
			for ( int c = 0; c < 3; c++ ) {
				const int at = k * 3 + c;

				p.ramp[at >> 2] |= (uint32_t)f->rampColors[k][c] << ( ( at & 3 ) * 8 );
			}
		}
		RP_NEXT( rp.filterPipe, VK_NULL_HANDLE, p );
	}

	if ( fxaa ) {
		struct {
			float rect[4];
		} p;

		RP_RectVec( &r, p.rect );
		RP_NEXT( rp.fxaaPipe, VK_NULL_HANDLE, p );
	}

	if ( sharpen ) {
		struct {
			float rect[4];
			float a[4];
		} p;

		RP_RectVec( &r, p.rect );
		p.a[0] = r_sharpen->value;
		p.a[1] = p.a[2] = p.a[3] = 0.0f;
		RP_NEXT( rp.casPipe, VK_NULL_HANDLE, p );
	}

#undef RP_NEXT

	// back into the view
	RP_Resume();
	RP_Quad( rp.copyPipe, src, VK_NULL_HANDLE, NULL, 0, &r );
	RP_Restore();
}
