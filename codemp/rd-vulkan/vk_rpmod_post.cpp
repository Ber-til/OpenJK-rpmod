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
//                             filter, SMAA or FXAA, sharpening; then the result back into the view
//
// Only with the post effects (r_fbo 1): the view is drawn into an image the passes can read. The
// shaders are glsl/rpmod/*, compiled by shaders/compile_rpmod.bat into shader_data_rpmod.c.

#include "tr_local.h"
#include "shaders/spirv/shader_data_rpmod.c"
#include "smaa/AreaTex.h"		// SMAA's tables (MIT licence: smaa/LICENSE.txt)
#include "smaa/SearchTex.h"
#include "tr_WorldEffects.h"
#include "../game/surfaceflags.h"

static cvar_t	*r_ssao;
static cvar_t	*r_ssaoRadius;
static cvar_t	*r_ssaoIntensity;
static cvar_t	*r_softParticles;
static cvar_t	*r_softParticlesDepth;
static cvar_t	*r_fxaa;
static cvar_t	*r_smaa;
static cvar_t	*r_water, *r_waterStrength, *r_waterRipples;
static cvar_t	*r_wetSurfaces, *r_wetStrength;
static cvar_t	*r_heatHaze, *r_heatHazeStrength;
static cvar_t	*r_skySharpen, *r_clouds, *r_cloudShadows, *r_aerialHaze, *r_aerialHazeStart, *r_skyScene[4];
static cvar_t	*r_sharpen;
static cvar_t	*r_relief, *r_reliefStrength, *r_reliefSpecular;
static cvar_t	*r_spriteLights;
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

#define RP_MAX_HAZE			12		// heat haze's sources a view, six a pass
#define RP_MAX_WET			4		// rain boxes a view whose ground is wet
#define RP_MAX_SKYOBJ		16		// the sky composer's objects
#define RP_SUNMAP_N			256		// the sun map's texels across at most
static void RP_SunMapForget( void );

static struct {
	qboolean			usable;			// the depth buffer can be read by a shader
	qboolean			ready;			// everything is there for this window

	VkShaderModule		depthFs, depthMsFs, dofFs, filterFs, copyFs;
	VkShaderModule		ssaoFs, aoBlurFs, aoApplyFs, shaftsFs[3], mblurFs, gradeFs, fxaaFs, casFs, volumFs, ssrFs, ssrBlurFs, shadowFs;
	VkShaderModule		smaaFs[3];		// edges, weights, blending
	VkShaderModule		fsrFs[2];		// upscaling, sharpening
	VkShaderModule		waterFs, wetFs, hazeFs, skyFs, aerialFs;
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

	// SMAA: the edges and their weights, the screen's size; its two tables, made once a window
	VkImage				smaaImage[2];
	VkImageView			smaaView[2];
	VkDeviceMemory		smaaMemory[2];
	VkImage				smaaTable[2];	// area, search
	VkImageView			smaaTableView[2];
	VkDeviceMemory		smaaTableMemory[2];

	// the shadow map: the models' depth from the light, and a colour image the pass needs beside it
	VkFormat			shadowFormat;
	VkImage				shadowImage, shadowColor;
	VkImageView			shadowView, shadowColorView;
	VkDeviceMemory		shadowMemory, shadowColorMemory;
	VkRenderPass		shadowPass;
	VkFramebuffer		shadowFb;
	VkDescriptorSet		shadowSet;
	qboolean			shadowSized;	// the shadow pass draws the shadow map now, not the shiny surfaces

	// reflections: the depth of the shiny surfaces alone, from the camera, the window's size (drawn
	// with the shadow map's render pass), to know which pixels show one
	VkImage				maskImage, maskColor;
	VkImageView			maskView, maskColorView;
	VkDeviceMemory		maskMemory, maskColorMemory;
	VkFramebuffer		maskFb;
	VkDescriptorSet		maskSet;
	qboolean			haveMask;		// this view's shiny surfaces are in it

	VkDescriptorSet		sceneSet, postSet[2], distSet, depthSet, aoSet[2];
	VkDescriptorSet		smaaSet[2], smaaTableSet[2];

	// FSR: the picture scaled up, then sharpened, the window's size (r_fsr)
	VkImage				fsrImage[2];
	VkImageView			fsrView[2];
	VkDeviceMemory		fsrMemory[2];
	VkFramebuffer		fsrFb[2];
	VkDescriptorSet		fsrSet[2];
	VkPipeline			fsrPipe[2];
	// and the rest of the frame after the last world view (the HUD, the console, the menus and their
	// models) drawn into the sharpened picture at the window's size: its depth, render pass, framebuffer
	VkImage				fsrDepth;
	VkImageView			fsrDepthView;
	VkDeviceMemory		fsrDepthMemory;
	VkRenderPass		fsrWindowPass;
	VkFramebuffer		fsrWindowFb;
	qboolean			fsrWindow;		// this frame's switched to it

	// water: the water surfaces' depth alone, from the camera, the window's size (drawn with the shadow
	// map's render pass, as the shiny surfaces are)
	VkImage				waterImage, waterColor;
	VkImageView			waterView, waterColorView;
	VkDeviceMemory		waterMemory, waterColorMemory;
	VkFramebuffer		waterFb;
	VkDescriptorSet		waterSet;
	qboolean			haveWater;		// this view's water surfaces are in it
	VkPipeline			waterPipe, wetPipe, hazePipe;

	// heat haze: what burns in this view, on the screen (x, y, radius in pixels, distance)
	vec4_t				haze[RP_MAX_HAZE];
	int					numHaze;

	// the sky: the composer's objects in a table, four texels each (r_skyScene, set by the game)
	VkImage				skyTable;
	VkImageView			skyTableView;
	VkDeviceMemory		skyTableMemory;
	VkDescriptorSet		skySet;
	// the sun map: where the sun reaches, for the cloud shadows (RP_SunMap)
	VkImage				sunMap;
	VkImageView			sunMapView;
	VkDeviceMemory		sunMapMemory;
	VkDescriptorSet		sunSet;
	VkPipeline			skyPipe, aerialPipe;
	int					skySceneCount;		// r_skyScene's modificationCount the table has
	int					numSkyObjects;
	float				skyTime;			// the composer's hour, -1 none
	float				skySunYaw;			// the compass side its sun is on, -1 auto (a Sun object's, else the map's)
	float				skyClouds;			// its cloud cover, -1 none
	qboolean			skyLightning;
	qboolean			skyComposed;		// the composer's own sky in place of the map's skybox
	qboolean			skyHasSun;			// a sun among its objects: the light's, for shafts and shadows
	vec3_t				skySunDir, skySunColor;
	qboolean			skyHasMoon;			// a moon among them: the light's once the sun's down
	vec3_t				skyMoonDir, skyMoonColor;
	qboolean			lightIsMoon;		// the light last found (RP_LightSun) is the moon's
	vec2_t				cloudDrift;			// how far the clouds have drifted
	int					cloudTime;
	int					flashUntil, nextFlash;
	qboolean			setsAllocated;
	VkDescriptorSet		skyResult;			// the post image the sky passes left the view in

	VkRenderPass		postPass, distPass, aoPass, smaaPass;
	VkFramebuffer		postFb[2], distFb, aoFb[2], smaaFb[2];

	VkPipeline			depthPipe, dofPipe, filterPipe, copyPipe;
	VkPipeline			ssaoPipe, aoBlurPipe, aoApplyPipe, shaftsPipe[3], mblurPipe, gradePipe, fxaaPipe, casPipe, volumPipe, ssrPipe, ssrBlurPipe, shadowPipe;
	VkPipeline			smaaPipe[3];

	qboolean			haveDistance;	// this view's distances are in distImage
	qboolean			softOn;			// and effects drawn now are faded by them

	// the main view's camera the frame before, for motion blur
	qboolean			prevValid;
	vec3_t				prevOrigin;
	vec3_t				prevAxis[3];
	int					prevTime;
	VkRect2D			prevRect;
} rp;

#define RP_NUM_SETS 18	// sceneSet, postSet[2], distSet, depthSet, aoSet[2], shadowSet, maskSet, smaaSet[2], smaaTableSet[2], fsrSet[2], waterSet, skySet, sunSet
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
	r_smaa = ri.Cvar_Get( "r_smaa", "0", CVAR_ARCHIVE_ND, va( "SMAA: smooths edges over the finished view, sharper than FXAA (in its place when both are on) and far cheaper than MSAA%s", fbo ) );
	ri.Cvar_CheckRange( r_smaa, 0, 1, qtrue );
	r_water = ri.Cvar_Get( "r_water", "0", CVAR_ARCHIVE_ND, va( "Water: the map's water ripples, bends and darkens what's under it with its depth, and mirrors the view more at a glancing look%s", fbo ) );
	ri.Cvar_CheckRange( r_water, 0, 1, qtrue );
	r_waterStrength = ri.Cvar_Get( "r_waterStrength", "1", CVAR_ARCHIVE_ND, "Water: how strong its look is, 0 to 2" );
	ri.Cvar_CheckRange( r_waterStrength, 0, 2, qfalse );
	r_waterRipples = ri.Cvar_Get( "r_waterRipples", "64", CVAR_ARCHIVE_ND, "Water: the size of its ripples, in world units" );
	ri.Cvar_CheckRange( r_waterRipples, 8, 512, qfalse );
	r_wetSurfaces = ri.Cvar_Get( "r_wetSurfaces", "0", CVAR_ARCHIVE_ND, va( "Wet surfaces: under the map editor's rain, the ground darkens, shines and gathers puddles that mirror the view, drying after%s", fbo ) );
	ri.Cvar_CheckRange( r_wetSurfaces, 0, 1, qtrue );
	r_wetStrength = ri.Cvar_Get( "r_wetStrength", "1", CVAR_ARCHIVE_ND, "Wet surfaces: how strongly puddles and the wet ground mirror, 0 to 2" );
	ri.Cvar_CheckRange( r_wetStrength, 0, 2, qfalse );
	r_heatHaze = ri.Cvar_Get( "r_heatHaze", "0", CVAR_ARCHIVE_ND, va( "Heat haze: the air shimmers over fire, explosions, thrusters and lava%s", fbo ) );
	ri.Cvar_CheckRange( r_heatHaze, 0, 1, qtrue );
	r_heatHazeStrength = ri.Cvar_Get( "r_heatHazeStrength", "3", CVAR_ARCHIVE_ND, "Heat haze: how far it bends what's behind, in pixels on a 480 high screen" );
	ri.Cvar_CheckRange( r_heatHazeStrength, 0, 12, qfalse );
	r_skySharpen = ri.Cvar_Get( "r_skySharpen", "0", CVAR_ARCHIVE_ND, va( "Sky sharpening: the map's skybox crisper, 0 to 1%s", fbo ) );
	ri.Cvar_CheckRange( r_skySharpen, 0, 1, qfalse );
	r_clouds = ri.Cvar_Get( "r_clouds", "1", CVAR_ARCHIVE_ND, va( "Clouds: 1 shows the clouds an admin composed in the sky (/skycomposer), drifting with the wind and lit by the sun; 0 hides them%s", fbo ) );
	ri.Cvar_CheckRange( r_clouds, 0, 1, qfalse );
	r_cloudShadows = ri.Cvar_Get( "r_cloudShadows", "0.5", CVAR_ARCHIVE_ND, "Clouds' shadows on the ground: how dark, 0 none to 1" );
	ri.Cvar_CheckRange( r_cloudShadows, 0, 1, qfalse );
	r_aerialHaze = ri.Cvar_Get( "r_aerialHaze", "0", CVAR_ARCHIVE_ND, va( "Aerial haze: far things fade into the sky's colour, less high up; how thick, 0 none to 4%s", fbo ) );
	ri.Cvar_CheckRange( r_aerialHaze, 0, 4, qfalse );
	r_aerialHazeStart = ri.Cvar_Get( "r_aerialHazeStart", "768", CVAR_ARCHIVE_ND, "Aerial haze: how far from you it starts, in world units" );
	ri.Cvar_CheckRange( r_aerialHazeStart, 0, 8192, qfalse );
	// the sky composer's scene, set by the game (/skycomposer): "sky composed|classic time <hour>
	// clouds <cover> lightning 1",
	// then objects: "<kind> <yaw> <pitch> <size> <r> <g> <b> [<seed> <tilt> <rings>]", ";" between
	// in four parts, a setting holding 256 characters at most
	for ( int i = 0; i < 4; i++ )
		r_skyScene[i] = ri.Cvar_Get( i ? va( "r_skyScene%i", i + 1 ) : "r_skyScene", "", CVAR_ROM | CVAR_TEMP, "The sky composer's scene (set by /skycomposer)" );
	r_relief = ri.Cvar_Get( "r_relief", "0", CVAR_ARCHIVE_ND | CVAR_LATCH, "Surface relief: dynamic lights and lit sabers pick out the relief of stone, metal and cloth, worked out from each texture (needs \\r_dlightMode 2; \\vid_restart to change)" );
	ri.Cvar_CheckRange( r_relief, 0, 1, qtrue );
	r_spriteLights = ri.Cvar_Get( "r_spriteLights", "0.7", CVAR_ARCHIVE_ND, "The map's grass and bushes (surface sprites) lit by dynamic lights, a saber's or a fire's, as the ground under them is: how much, 0 not at all" );
	ri.Cvar_CheckRange( r_spriteLights, 0, 4, qfalse );
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
	r_ssr = ri.Cvar_Get( "r_ssr", "0", CVAR_ARCHIVE_ND, va( "Reflections: shiny floors (marble, tiles, metal, glass, ice, water, by the map's materials) mirror what's on screen%s", fbo ) );
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
	r_shadowSun = ri.Cvar_Get( "r_shadowSun", "0", CVAR_ARCHIVE_ND, "Real shadows: 0 cast nearly straight down, which suits indoors; 1 from the map's sun, on maps whose sky has one (a sky composer's sun is followed either way)" );
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

#ifdef USE_VBO_SS
// the surface sprites' vertex shaders swapped for the same lit by the view's dynamic lights
// (surface_sprite_vert.tmpl built with USE_DLIGHTS); without them, r_spriteLights 0 lights none
void vk_rpmod_sprite_modules( void ) {
	for ( int i = 0; i < 2; i++ ) {
		if ( vk.shaders.surface_sprite_vs[i] != VK_NULL_HANDLE )
			qvkDestroyShaderModule( vk.device, vk.shaders.surface_sprite_vs[i], NULL );
	}
	RP_MODULE( vk.shaders.surface_sprite_vs[0], rpmod_ss_dlight_vert_spv, "rpmod surface sprite vertex" );
	RP_MODULE( vk.shaders.surface_sprite_vs[1], rpmod_ss_dlight_fog_vert_spv, "rpmod surface sprite fog vertex" );
}
#endif

// how bright a dynamic light is drawn, as the light passes have it
static void RP_DlightColor( const dlight_t *dl, vec3_t out ) {
	if ( !glConfig.deviceSupportsGamma && !vk.fboActive )
		VectorScale( dl->color, 2 * powf( r_intensity->value, r_gamma->value ), out );
	else
		VectorCopy( dl->color, out );
}

// the view's dynamic lights nearest the eye, for the surface sprites (the camera's uniform)
void vk_rpmod_camera_dlights( vkUniformCamera_t *u, const trRefdef_t *refdef ) {
	int order[VK_CAMERA_DLIGHTS], count = 0;
	float howFar[VK_CAMERA_DLIGHTS];
	const float strength = r_spriteLights ? r_spriteLights->value : 0.0f;

	u->dlightCount[0] = 0.0f;
	if ( strength <= 0.0f || !refdef->dlights )
		return;
	for ( int i = 0; i < refdef->num_dlights; i++ ) {
		const dlight_t *dl = &refdef->dlights[i];
		const float d = Distance( dl->origin, refdef->vieworg ) - dl->radius;
		int at = count;

		if ( dl->radius <= 0.0f )
			continue;
		// kept nearest first: a farther one in a full list is left out
		while ( at > 0 && howFar[at - 1] > d )
			at--;
		if ( at >= VK_CAMERA_DLIGHTS )
			continue;
		for ( int j = Q_min( count, VK_CAMERA_DLIGHTS - 1 ); j > at; j-- ) {
			howFar[j] = howFar[j - 1];
			order[j] = order[j - 1];
		}
		howFar[at] = d;
		order[at] = i;
		count = Q_min( count + 1, VK_CAMERA_DLIGHTS );
	}
	for ( int i = 0; i < count; i++ ) {
		const dlight_t *dl = &refdef->dlights[order[i]];

		VectorCopy( dl->origin, u->dlightPos[i] );
		u->dlightPos[i][3] = 1.0f / Square( dl->radius );
		RP_DlightColor( dl, u->dlightColor[i] );
		VectorScale( u->dlightColor[i], strength, u->dlightColor[i] );
		u->dlightColor[i][3] = 0.0f;
	}
	u->dlightCount[0] = (float)count;
}

// a surface sprite drawn without the GPU's sprites (tr_surfacesprites.cpp): the same light added to
// its colour, where it stands
void RB_SurfaceSpriteDlights( byte *color, const vec3_t loc ) {
	const float strength = r_spriteLights ? r_spriteLights->value : 0.0f;

	if ( strength <= 0.0f )
		return;
	for ( int i = 0; i < backEnd.refdef.num_dlights; i++ ) {
		const dlight_t *dl = &backEnd.refdef.dlights[i];
		vec3_t L, c;
		float f;

		if ( dl->radius <= 0.0f )
			continue;
		VectorSubtract( dl->origin, loc, L );
		f = 1.0f - DotProduct( L, L ) / Square( dl->radius );
		if ( f <= 0.0f )
			continue;
		RP_DlightColor( dl, c );
		for ( int k = 0; k < 3; k++ ) {
			const float v = color[k] + c[k] * f * strength * 255.0f;

			color[k] = (byte)( v > 255.0f ? 255 : v );
		}
	}
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
	VkDescriptorSetLayout setLayouts[3] = { vk.set_layout_sampler, vk.set_layout_sampler, vk.set_layout_sampler };
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
	RP_MODULE( rp.ssrBlurFs, rpmod_ssrblur_frag_spv, "rpmod reflections softened" );
	RP_MODULE( rp.shadowFs, rpmod_shadow_frag_spv, "rpmod real shadows" );
	RP_MODULE( rp.smaaFs[0], rpmod_smaa_edges_frag_spv, "rpmod smaa edges" );
	RP_MODULE( rp.smaaFs[1], rpmod_smaa_weights_frag_spv, "rpmod smaa weights" );
	RP_MODULE( rp.smaaFs[2], rpmod_smaa_blend_frag_spv, "rpmod smaa blending" );
	RP_MODULE( rp.fsrFs[0], rpmod_fsr_easu_frag_spv, "rpmod fsr upscaling" );
	RP_MODULE( rp.fsrFs[1], rpmod_fsr_rcas_frag_spv, "rpmod fsr sharpening" );
	RP_MODULE( rp.waterFs, rpmod_water_frag_spv, "rpmod water" );
	RP_MODULE( rp.wetFs, rpmod_wet_frag_spv, "rpmod wet surfaces" );
	RP_MODULE( rp.hazeFs, rpmod_haze_frag_spv, "rpmod heat haze" );
	RP_MODULE( rp.skyFs, rpmod_sky_frag_spv, "rpmod sky" );
	RP_MODULE( rp.aerialFs, rpmod_aerial_frag_spv, "rpmod aerial haze" );
	rp.skySceneCount = -1;
	rp.skyTime = rp.skyClouds = -1.0f;

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
		&rp.shaftsFs[0], &rp.shaftsFs[1], &rp.shaftsFs[2], &rp.mblurFs, &rp.gradeFs, &rp.fxaaFs, &rp.casFs, &rp.volumFs, &rp.ssrFs, &rp.ssrBlurFs, &rp.shadowFs,
		&rp.smaaFs[0], &rp.smaaFs[1], &rp.smaaFs[2], &rp.fsrFs[0], &rp.fsrFs[1], &rp.waterFs, &rp.wetFs, &rp.hazeFs, &rp.skyFs, &rp.aerialFs };

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
static void RP_CreateImageSized( VkFormat format, uint32_t width, uint32_t height, VkImage *image, VkImageView *view, VkDeviceMemory *memory, const char *name ) {
	VkImageCreateInfo desc;
	VkMemoryRequirements req;
	VkMemoryAllocateInfo alloc;
	VkImageViewCreateInfo vdesc;

	Com_Memset( &desc, 0, sizeof( desc ) );
	desc.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	desc.imageType = VK_IMAGE_TYPE_2D;
	desc.format = format;
	desc.extent.width = width;
	desc.extent.height = height;
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

// one the picture's size
static void RP_CreateImage( VkFormat format, VkImage *image, VkImageView *view, VkDeviceMemory *memory, const char *name ) {
	RP_CreateImageSized( format, glConfig.vidWidth, glConfig.vidHeight, image, view, memory, name );
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
	VkDescriptorSet *sets[RP_NUM_SETS] = { &rp.sceneSet, &rp.postSet[0], &rp.postSet[1], &rp.distSet, &rp.depthSet, &rp.aoSet[0], &rp.aoSet[1], &rp.shadowSet, &rp.maskSet,
		&rp.smaaSet[0], &rp.smaaSet[1], &rp.smaaTableSet[0], &rp.smaaTableSet[1], &rp.fsrSet[0], &rp.fsrSet[1], &rp.waterSet, &rp.skySet, &rp.sunSet };

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
	rp.sceneSet = rp.postSet[0] = rp.postSet[1] = rp.distSet = rp.depthSet = rp.aoSet[0] = rp.aoSet[1] = rp.shadowSet = rp.maskSet = VK_NULL_HANDLE;
	rp.smaaSet[0] = rp.smaaSet[1] = rp.smaaTableSet[0] = rp.smaaTableSet[1] = rp.fsrSet[0] = rp.fsrSet[1] = rp.waterSet = rp.skySet = rp.sunSet = VK_NULL_HANDLE;
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
	if ( rp.maskView != VK_NULL_HANDLE )
		RP_SetImage( rp.maskSet, rp.maskView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, GL_NEAREST );
	// SMAA reads the edges and the area table between pixels, the search table on them
	if ( rp.smaaView[0] != VK_NULL_HANDLE ) {
		RP_SetImage( rp.smaaSet[0], rp.smaaView[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, GL_LINEAR );
		RP_SetImage( rp.smaaSet[1], rp.smaaView[1], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, GL_LINEAR );
	}
	if ( rp.smaaTableView[0] != VK_NULL_HANDLE ) {
		RP_SetImage( rp.smaaTableSet[0], rp.smaaTableView[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, GL_LINEAR );
		RP_SetImage( rp.smaaTableSet[1], rp.smaaTableView[1], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, GL_NEAREST );
	}
	if ( rp.waterView != VK_NULL_HANDLE )
		RP_SetImage( rp.waterSet, rp.waterView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, GL_NEAREST );
	if ( rp.skyTableView != VK_NULL_HANDLE )
		RP_SetImage( rp.skySet, rp.skyTableView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, GL_NEAREST );
	if ( rp.sunMapView != VK_NULL_HANDLE )
		RP_SetImage( rp.sunSet, rp.sunMapView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, GL_NEAREST );
	// FSR's: read a pixel at a time, and shown one to one by the gamma pass
	if ( rp.fsrView[0] != VK_NULL_HANDLE ) {
		RP_SetImage( rp.fsrSet[0], rp.fsrView[0], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, GL_NEAREST );
		RP_SetImage( rp.fsrSet[1], rp.fsrView[1], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, GL_NEAREST );
	}
}

/*
===============
SMAA's two tables: images of their own, filled from the bytes once a window
===============
*/
static void RP_CreateTable( VkFormat format, uint32_t width, uint32_t height, const unsigned char *bytes, size_t size,
	VkImage *image, VkImageView *view, VkDeviceMemory *memory, const char *name ) {
	VkImageCreateInfo desc;
	VkMemoryRequirements req;
	VkMemoryAllocateInfo alloc;
	VkImageViewCreateInfo vdesc;
	VkBufferCreateInfo bdesc;
	VkBuffer staging;
	VkDeviceMemory stagingMemory;
	VkBufferImageCopy region;
	VkCommandBuffer cmd;
	void *data;

	Com_Memset( &desc, 0, sizeof( desc ) );
	desc.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	desc.imageType = VK_IMAGE_TYPE_2D;
	desc.format = format;
	desc.extent.width = width;
	desc.extent.height = height;
	desc.extent.depth = 1;
	desc.mipLevels = 1;
	desc.arrayLayers = 1;
	desc.samples = VK_SAMPLE_COUNT_1_BIT;
	desc.tiling = VK_IMAGE_TILING_OPTIMAL;
	desc.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
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

	// the bytes through a buffer the CPU can write
	Com_Memset( &bdesc, 0, sizeof( bdesc ) );
	bdesc.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bdesc.size = size;
	bdesc.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	bdesc.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VK_CHECK( qvkCreateBuffer( vk.device, &bdesc, NULL, &staging ) );
	qvkGetBufferMemoryRequirements( vk.device, staging, &req );
	alloc.allocationSize = req.size;
	alloc.memoryTypeIndex = vk_find_memory_type( req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT );
	VK_CHECK( qvkAllocateMemory( vk.device, &alloc, NULL, &stagingMemory ) );
	VK_CHECK( qvkBindBufferMemory( vk.device, staging, stagingMemory, 0 ) );
	VK_CHECK( qvkMapMemory( vk.device, stagingMemory, 0, VK_WHOLE_SIZE, 0, &data ) );
	Com_Memcpy( data, bytes, size );
	qvkUnmapMemory( vk.device, stagingMemory );

	Com_Memset( &region, 0, sizeof( region ) );
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.layerCount = 1;
	region.imageExtent.width = width;
	region.imageExtent.height = height;
	region.imageExtent.depth = 1;
	cmd = vk_begin_command_buffer();
	vk_record_image_layout_transition( cmd, *image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 0 );
	qvkCmdCopyBufferToImage( cmd, staging, *image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region );
	vk_record_image_layout_transition( cmd, *image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 0 );
	vk_end_command_buffer( cmd, __func__ );
	qvkDestroyBuffer( vk.device, staging, NULL );
	qvkFreeMemory( vk.device, stagingMemory, NULL );

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

static void RP_CreateShadowImage( VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, VkImage *image, VkImageView *view, VkDeviceMemory *memory, const char *name,
	uint32_t width = RP_SHADOW_SIZE, uint32_t height = RP_SHADOW_SIZE ) {
	VkImageCreateInfo desc;
	VkMemoryRequirements req;
	VkMemoryAllocateInfo alloc;
	VkImageViewCreateInfo vdesc;

	Com_Memset( &desc, 0, sizeof( desc ) );
	desc.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	desc.imageType = VK_IMAGE_TYPE_2D;
	desc.format = format;
	desc.extent.width = width;
	desc.extent.height = height;
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

// the shadow pass's viewport: all of the shadow map; qfalse while it draws the shiny surfaces, the view's own
qboolean vk_rpmod_shadow_viewport( VkRect2D *r ) {
	if ( !rp.shadowSized )
		return qfalse;
	r->offset.x = r->offset.y = 0;
	r->extent.width = r->extent.height = RP_SHADOW_SIZE;
	return qtrue;
}

void vk_rpmod_create_attachments( void ) {
	VkImageViewCreateInfo vdesc;
	VkCommandBuffer cmd;
	VkImage *images[] = { &rp.postImage[0], &rp.postImage[1], &rp.distImage, &rp.aoImage[0], &rp.aoImage[1], &rp.smaaImage[0], &rp.smaaImage[1],
		&rp.fsrImage[0], &rp.fsrImage[1] };

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
	RP_CreateImage( VK_FORMAT_R8G8B8A8_UNORM, &rp.smaaImage[0], &rp.smaaView[0], &rp.smaaMemory[0], "rpmod smaa edges" );
	RP_CreateImage( VK_FORMAT_R8G8B8A8_UNORM, &rp.smaaImage[1], &rp.smaaView[1], &rp.smaaMemory[1], "rpmod smaa weights" );
	RP_CreateTable( VK_FORMAT_R8G8_UNORM, AREATEX_WIDTH, AREATEX_HEIGHT, areaTexBytes, sizeof( areaTexBytes ),
		&rp.smaaTable[0], &rp.smaaTableView[0], &rp.smaaTableMemory[0], "rpmod smaa area table" );
	RP_CreateTable( VK_FORMAT_R8_UNORM, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, searchTexBytes, sizeof( searchTexBytes ),
		&rp.smaaTable[1], &rp.smaaTableView[1], &rp.smaaTableMemory[1], "rpmod smaa search table" );
	// the sky composer's table, empty until there's a scene
	{
		static float none[RP_MAX_SKYOBJ * 4 * 4];

		RP_CreateTable( VK_FORMAT_R32G32B32A32_SFLOAT, RP_MAX_SKYOBJ * 4, 1, (const unsigned char *)none, sizeof( none ),
			&rp.skyTable, &rp.skyTableView, &rp.skyTableMemory, "rpmod sky objects" );
		rp.skySceneCount = -1;
	}
	// the sun map, empty (not ready) until one's made
	{
		static float none[RP_SUNMAP_N * ( RP_SUNMAP_N + 1 )];

		RP_CreateTable( VK_FORMAT_R32_SFLOAT, RP_SUNMAP_N, RP_SUNMAP_N + 1, (const unsigned char *)none, sizeof( none ),
			&rp.sunMap, &rp.sunMapView, &rp.sunMapMemory, "rpmod sun map" );
		RP_SunMapForget();
	}
	// FSR's, only when the picture is drawn smaller than the window for it
	if ( r_fsr->integer && !r_renderScale->integer && ( glConfig.vidWidth != gls.windowWidth || glConfig.vidHeight != gls.windowHeight ) ) {
		RP_CreateImageSized( vk.color_format, gls.windowWidth, gls.windowHeight, &rp.fsrImage[0], &rp.fsrView[0], &rp.fsrMemory[0], "rpmod fsr upscaled" );
		RP_CreateImageSized( vk.color_format, gls.windowWidth, gls.windowHeight, &rp.fsrImage[1], &rp.fsrView[1], &rp.fsrMemory[1], "rpmod fsr sharpened" );
		// the 2D after it at the window's size: not with MSAA (its own multisampled image would be needed)
		if ( !vk.msaaActive )
			RP_CreateShadowImage( vk.depth_format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
				VK_IMAGE_ASPECT_DEPTH_BIT | ( glConfig.stencilBits > 0 ? VK_IMAGE_ASPECT_STENCIL_BIT : 0 ),
				&rp.fsrDepth, &rp.fsrDepthView, &rp.fsrDepthMemory, "rpmod fsr window depth", gls.windowWidth, gls.windowHeight );
	}
	rp.shadowFormat = RP_ShadowFormat();
	if ( rp.shadowFormat != VK_FORMAT_UNDEFINED ) {
		RP_CreateShadowImage( rp.shadowFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT,
			&rp.shadowImage, &rp.shadowView, &rp.shadowMemory, "rpmod shadow map" );
		RP_CreateShadowImage( VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
			&rp.shadowColor, &rp.shadowColorView, &rp.shadowColorMemory, "rpmod shadow map colour" );
		RP_CreateShadowImage( rp.shadowFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT,
			&rp.maskImage, &rp.maskView, &rp.maskMemory, "rpmod shiny surfaces", glConfig.vidWidth, glConfig.vidHeight );
		RP_CreateShadowImage( VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
			&rp.maskColor, &rp.maskColorView, &rp.maskColorMemory, "rpmod shiny surfaces colour", glConfig.vidWidth, glConfig.vidHeight );
		RP_CreateShadowImage( rp.shadowFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT,
			&rp.waterImage, &rp.waterView, &rp.waterMemory, "rpmod water surfaces", glConfig.vidWidth, glConfig.vidHeight );
		RP_CreateShadowImage( VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
			&rp.waterColor, &rp.waterColorView, &rp.waterColorMemory, "rpmod water surfaces colour", glConfig.vidWidth, glConfig.vidHeight );
	}

	// as the render passes have them before and after
	cmd = vk_begin_command_buffer();
	for ( size_t i = 0; i < ARRAY_LEN( images ); i++ ) {
		if ( *images[i] != VK_NULL_HANDLE )
			vk_record_image_layout_transition( cmd, *images[i], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 0 );
	}
	if ( rp.shadowImage != VK_NULL_HANDLE )
		vk_record_image_layout_transition( cmd, rp.shadowImage, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 0 );
	if ( rp.maskImage != VK_NULL_HANDLE )
		vk_record_image_layout_transition( cmd, rp.maskImage, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 0 );
	if ( rp.waterImage != VK_NULL_HANDLE )
		vk_record_image_layout_transition( cmd, rp.waterImage, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 0 );
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
	for ( int i = 0; i < 2; i++ ) {
		RP_DestroyImage( &rp.smaaImage[i], &rp.smaaView[i], &rp.smaaMemory[i] );
		RP_DestroyImage( &rp.smaaTable[i], &rp.smaaTableView[i], &rp.smaaTableMemory[i] );
		RP_DestroyImage( &rp.fsrImage[i], &rp.fsrView[i], &rp.fsrMemory[i] );
	}
	RP_DestroyImage( &rp.fsrDepth, &rp.fsrDepthView, &rp.fsrDepthMemory );
	RP_DestroyImage( &rp.shadowImage, &rp.shadowView, &rp.shadowMemory );
	RP_DestroyImage( &rp.shadowColor, &rp.shadowColorView, &rp.shadowColorMemory );
	RP_DestroyImage( &rp.maskImage, &rp.maskView, &rp.maskMemory );
	RP_DestroyImage( &rp.maskColor, &rp.maskColorView, &rp.maskColorMemory );
	RP_DestroyImage( &rp.waterImage, &rp.waterView, &rp.waterMemory );
	RP_DestroyImage( &rp.skyTable, &rp.skyTableView, &rp.skyTableMemory );
	RP_DestroyImage( &rp.sunMap, &rp.sunMapView, &rp.sunMapMemory );
	RP_DestroyImage( &rp.waterColor, &rp.waterColorView, &rp.waterColorMemory );
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

// FSR's window: the sharpened picture kept and drawn over at the window's size, a depth of its own
// cleared; like the view's own render pass (its colour and depth formats), so its pipelines draw in it
static VkRenderPass RP_CreateFsrWindowPass( void ) {
	VkAttachmentDescription att[2];
	VkAttachmentReference color, depth;
	VkSubpassDescription sub;
	VkSubpassDependency deps[3];
	VkRenderPassCreateInfo desc;
	VkRenderPass pass;

	Com_Memset( att, 0, sizeof( att ) );
	att[0].format = vk.color_format;
	att[0].samples = VK_SAMPLE_COUNT_1_BIT;
	att[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	att[0].initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	att[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	att[1].format = vk.depth_format;
	att[1].samples = VK_SAMPLE_COUNT_1_BIT;
	att[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	att[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	att[1].stencilLoadOp = glConfig.stencilBits > 0 ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	att[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	color.attachment = 0;
	color.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	depth.attachment = 1;
	depth.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	Com_Memset( &sub, 0, sizeof( sub ) );
	sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	sub.colorAttachmentCount = 1;
	sub.pColorAttachments = &color;
	sub.pDepthStencilAttachment = &depth;

	deps[0] = vk.render_pass.subpass_deps.shader_to_color;
	deps[1] = vk.render_pass.subpass_deps.color_to_shader;
	// the depth, after the last frame's use of it
	Com_Memset( &deps[2], 0, sizeof( deps[2] ) );
	deps[2].srcSubpass = VK_SUBPASS_EXTERNAL;
	deps[2].dstSubpass = 0;
	deps[2].srcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	deps[2].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	deps[2].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	deps[2].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

	Com_Memset( &desc, 0, sizeof( desc ) );
	desc.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	desc.attachmentCount = 2;
	desc.pAttachments = att;
	desc.subpassCount = 1;
	desc.pSubpasses = &sub;
	desc.dependencyCount = ARRAY_LEN( deps );
	desc.pDependencies = deps;
	VK_CHECK( qvkCreateRenderPass( vk.device, &desc, NULL, &pass ) );
	VK_SET_OBJECT_NAME( pass, "render pass - rpmod fsr window", VK_DEBUG_REPORT_OBJECT_TYPE_RENDER_PASS_EXT );
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
	rp.smaaPass = RP_CreatePass( VK_FORMAT_R8G8B8A8_UNORM, "render pass - rpmod smaa" );
	if ( rp.fsrDepth != VK_NULL_HANDLE )
		rp.fsrWindowPass = RP_CreateFsrWindowPass();
}

void vk_rpmod_destroy_render_passes( void ) {
	VkRenderPass *passes[] = { &rp.postPass, &rp.distPass, &rp.aoPass, &rp.shadowPass, &rp.smaaPass, &rp.fsrWindowPass };

	rp.ready = qfalse;
	for ( size_t i = 0; i < ARRAY_LEN( passes ); i++ ) {
		if ( *passes[i] != VK_NULL_HANDLE )
			qvkDestroyRenderPass( vk.device, *passes[i], NULL );
		*passes[i] = VK_NULL_HANDLE;
	}
}

static VkFramebuffer RP_CreateFramebufferSized( VkRenderPass pass, VkImageView view, uint32_t width, uint32_t height, const char *name ) {
	VkFramebufferCreateInfo desc;
	VkFramebuffer fb;

	Com_Memset( &desc, 0, sizeof( desc ) );
	desc.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	desc.renderPass = pass;
	desc.attachmentCount = 1;
	desc.pAttachments = &view;
	desc.width = width;
	desc.height = height;
	desc.layers = 1;
	VK_CHECK( qvkCreateFramebuffer( vk.device, &desc, NULL, &fb ) );
	VK_SET_OBJECT_NAME( fb, name, VK_DEBUG_REPORT_OBJECT_TYPE_FRAMEBUFFER_EXT );
	return fb;
}

static VkFramebuffer RP_CreateFramebuffer( VkRenderPass pass, VkImageView view, const char *name ) {
	return RP_CreateFramebufferSized( pass, view, glConfig.vidWidth, glConfig.vidHeight, name );
}

void vk_rpmod_create_framebuffers( void ) {
	if ( rp.postPass == VK_NULL_HANDLE )
		return;
	rp.postFb[0] = RP_CreateFramebuffer( rp.postPass, rp.postView[0], "framebuffer - rpmod post 0" );
	rp.postFb[1] = RP_CreateFramebuffer( rp.postPass, rp.postView[1], "framebuffer - rpmod post 1" );
	rp.distFb = RP_CreateFramebuffer( rp.distPass, rp.distView, "framebuffer - rpmod distance" );
	rp.aoFb[0] = RP_CreateFramebuffer( rp.aoPass, rp.aoView[0], "framebuffer - rpmod ambient occlusion 0" );
	rp.aoFb[1] = RP_CreateFramebuffer( rp.aoPass, rp.aoView[1], "framebuffer - rpmod ambient occlusion 1" );
	rp.smaaFb[0] = RP_CreateFramebuffer( rp.smaaPass, rp.smaaView[0], "framebuffer - rpmod smaa edges" );
	rp.smaaFb[1] = RP_CreateFramebuffer( rp.smaaPass, rp.smaaView[1], "framebuffer - rpmod smaa weights" );
	// the post images' render pass: the same colour format
	if ( rp.fsrView[0] != VK_NULL_HANDLE ) {
		rp.fsrFb[0] = RP_CreateFramebufferSized( rp.postPass, rp.fsrView[0], gls.windowWidth, gls.windowHeight, "framebuffer - rpmod fsr upscaled" );
		rp.fsrFb[1] = RP_CreateFramebufferSized( rp.postPass, rp.fsrView[1], gls.windowWidth, gls.windowHeight, "framebuffer - rpmod fsr sharpened" );
		if ( rp.fsrWindowPass != VK_NULL_HANDLE ) {
			VkImageView views[2] = { rp.fsrView[1], rp.fsrDepthView };
			VkFramebufferCreateInfo desc;

			Com_Memset( &desc, 0, sizeof( desc ) );
			desc.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
			desc.renderPass = rp.fsrWindowPass;
			desc.attachmentCount = 2;
			desc.pAttachments = views;
			desc.width = gls.windowWidth;
			desc.height = gls.windowHeight;
			desc.layers = 1;
			VK_CHECK( qvkCreateFramebuffer( vk.device, &desc, NULL, &rp.fsrWindowFb ) );
			VK_SET_OBJECT_NAME( rp.fsrWindowFb, "framebuffer - rpmod fsr window", VK_DEBUG_REPORT_OBJECT_TYPE_FRAMEBUFFER_EXT );
		}
	}
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
		if ( rp.maskImage != VK_NULL_HANDLE ) {
			views[0] = rp.maskColorView;
			views[1] = rp.maskView;
			desc.width = glConfig.vidWidth;
			desc.height = glConfig.vidHeight;
			VK_CHECK( qvkCreateFramebuffer( vk.device, &desc, NULL, &rp.maskFb ) );
			VK_SET_OBJECT_NAME( rp.maskFb, "framebuffer - rpmod shiny surfaces", VK_DEBUG_REPORT_OBJECT_TYPE_FRAMEBUFFER_EXT );
		}
		if ( rp.waterImage != VK_NULL_HANDLE ) {
			views[0] = rp.waterColorView;
			views[1] = rp.waterView;
			desc.width = glConfig.vidWidth;
			desc.height = glConfig.vidHeight;
			VK_CHECK( qvkCreateFramebuffer( vk.device, &desc, NULL, &rp.waterFb ) );
			VK_SET_OBJECT_NAME( rp.waterFb, "framebuffer - rpmod water surfaces", VK_DEBUG_REPORT_OBJECT_TYPE_FRAMEBUFFER_EXT );
		}
	}
}

void vk_rpmod_destroy_framebuffers( void ) {
	VkFramebuffer *fbs[] = { &rp.postFb[0], &rp.postFb[1], &rp.distFb, &rp.aoFb[0], &rp.aoFb[1], &rp.shadowFb, &rp.maskFb, &rp.smaaFb[0], &rp.smaaFb[1], &rp.fsrFb[0], &rp.fsrFb[1], &rp.waterFb, &rp.fsrWindowFb };

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
		&rp.fxaaPipe, &rp.casPipe, &rp.volumPipe, &rp.ssrPipe, &rp.ssrBlurPipe, &rp.shadowPipe, &rp.smaaPipe[0], &rp.smaaPipe[1], &rp.smaaPipe[2], &rp.fsrPipe[0], &rp.fsrPipe[1], &rp.waterPipe, &rp.wetPipe, &rp.hazePipe, &rp.skyPipe, &rp.aerialPipe };

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
	// the reflections alone into the SMAA edges' image (free till SMAA, later), then softened onto the view
	rp.ssrPipe = RP_CreatePipeline( rp.ssrFs, rp.smaaPass, one, qfalse, "rpmod reflections" );
	rp.ssrBlurPipe = RP_CreatePipeline( rp.ssrBlurFs, rp.postPass, one, qfalse, "rpmod reflections softened" );
	rp.smaaPipe[0] = RP_CreatePipeline( rp.smaaFs[0], rp.smaaPass, one, qfalse, "rpmod smaa edges" );
	rp.smaaPipe[1] = RP_CreatePipeline( rp.smaaFs[1], rp.smaaPass, one, qfalse, "rpmod smaa weights" );
	rp.smaaPipe[2] = RP_CreatePipeline( rp.smaaFs[2], rp.postPass, one, qfalse, "rpmod smaa blending" );
	rp.fsrPipe[0] = RP_CreatePipeline( rp.fsrFs[0], rp.postPass, one, qfalse, "rpmod fsr upscaling" );
	rp.fsrPipe[1] = RP_CreatePipeline( rp.fsrFs[1], rp.postPass, one, qfalse, "rpmod fsr sharpening" );
	rp.waterPipe = RP_CreatePipeline( rp.waterFs, rp.postPass, one, qfalse, "rpmod water" );
	rp.wetPipe = RP_CreatePipeline( rp.wetFs, rp.postPass, one, qfalse, "rpmod wet surfaces" );
	rp.hazePipe = RP_CreatePipeline( rp.hazeFs, rp.postPass, one, qfalse, "rpmod heat haze" );
	rp.skyPipe = RP_CreatePipeline( rp.skyFs, rp.postPass, one, qfalse, "rpmod sky" );
	rp.aerialPipe = RP_CreatePipeline( rp.aerialFs, rp.postPass, one, qfalse, "rpmod aerial haze" );
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

static void RP_Quad3( VkPipeline pipe, VkDescriptorSet set0, VkDescriptorSet set1, VkDescriptorSet set2, const void *push, uint32_t pushSize, const VkRect2D *r ) {
	VkDescriptorSet sets[3] = { set0, set1, set2 };

	qvkCmdBindPipeline( vk.cmd->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe );
	qvkCmdBindDescriptorSets( vk.cmd->command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, rp.layout, 0, set2 != VK_NULL_HANDLE ? 3 : set1 != VK_NULL_HANDLE ? 2 : 1, sets, 0, NULL );
	if ( push )
		qvkCmdPushConstants( vk.cmd->command_buffer, rp.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, pushSize, push );
	RP_SetViewport( r );
	qvkCmdDraw( vk.cmd->command_buffer, 4, 1, 0, 0 );
}

// one pass into an image of our own
static void RP_Quad( VkPipeline pipe, VkDescriptorSet set0, VkDescriptorSet set1, const void *push, uint32_t pushSize, const VkRect2D *r ) {
	RP_Quad3( pipe, set0, set1, VK_NULL_HANDLE, push, pushSize, r );
}

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
	return (qboolean)( rp.ready && rp.setsAllocated && vk.fboActive && vk.renderPassIndex != RENDER_PASS_SCREENMAP && !rp.fsrWindow
		&& backEnd.viewParms.portalView == PV_NONE && !( backEnd.refdef.rdflags & ( RDF_NOWORLDMODEL | RDF_SKYBOXPORTAL ) ) );
}

/*
===============
the sun, for sun shafts: where it is in the view and how much its beams show; qfalse when the
map's sky has no sun, or it's behind the camera or too far off the side of the view
===============
*/
static void RP_SkySun( vec3_t out );

/*
the sun that lights, for the sun shafts and real shadows: the sky composer's (its Sun object, else the
sun of its hour), else the map's; at night the composer's Moon object, once the sun's light is
less than the moon's: dim and cool, from where the moon is. qfalse when there's none up
*/
#define RP_MOONLIGHT		0.3f	// the moon's light, of the sun's
#define RP_MOON_SHAFTS		0.4f	// its shafts, of the sun's
#define RP_MOON_SHADOWS		0.6f	// its shadows' darkness, of the sun's
#define RP_MOON_CLOUDS		0.5f	// its clouds' shadows, of the sun's

static qboolean RP_LightSunOnly( vec3_t dir, vec3_t color );

static qboolean RP_LightSun( vec3_t dir, vec3_t color ) {
	const qboolean sun = RP_LightSunOnly( dir, color );

	rp.lightIsMoon = qfalse;
	if ( rp.skyHasMoon && rp.skyMoonDir[2] > 0.0f ) {
		const float up = Com_Clamp( 0.0f, 1.0f, rp.skyMoonDir[2] / 0.15f );
		const float moon = RP_MOONLIGHT * up;
		const float sunNow = sun && dir[2] > 0.0f ? MAX( color[0], MAX( color[1], color[2] ) ) * Com_Clamp( 0.0f, 1.0f, dir[2] / 0.1f ) : 0.0f;

		if ( moon > 0.0f && sunNow < moon ) {
			const float m = MAX( rp.skyMoonColor[0], MAX( rp.skyMoonColor[1], rp.skyMoonColor[2] ) );
			static const vec3_t cool = { 0.75f, 0.85f, 1.0f };

			VectorCopy( rp.skyMoonDir, dir );
			VectorNormalize( dir );
			for ( int i = 0; i < 3; i++ )
				color[i] = ( m > 0.0f ? rp.skyMoonColor[i] / m : 1.0f ) * cool[i] * moon;
			rp.lightIsMoon = qtrue;
			return qtrue;
		}
	}
	return sun;
}

static qboolean RP_LightSunOnly( vec3_t dir, vec3_t color ) {
	if ( rp.skyHasSun ) {
		VectorCopy( rp.skySunDir, dir );
		VectorCopy( rp.skySunColor, color );
	} else if ( rp.skyTime >= 0.0f ) {
		const float m = MAX( tr.sunLight[0], MAX( tr.sunLight[1], tr.sunLight[2] ) );

		RP_SkySun( dir );
		if ( m > 0.0f )
			VectorScale( tr.sunLight, 1.0f / m, color );
		else
			VectorSet( color, 1.0f, 0.97f, 0.9f );
		// orange low, gone below the horizon
		{
			const float low = 1.0f - Com_Clamp( 0.0f, 1.0f, dir[2] / 0.4f );

			color[1] *= 1.0f - 0.5f * low;
			color[2] *= 1.0f - 0.75f * low;
			VectorScale( color, Com_Clamp( 0.0f, 1.0f, ( dir[2] + 0.05f ) / 0.15f ), color );
		}
	} else {
		VectorCopy( tr.sunDirection, dir );
		VectorCopy( tr.sunLight, color );
	}
	VectorNormalize( dir );
	return (qboolean)( MAX( color[0], MAX( color[1], color[2] ) ) > 0.0f );
}

qboolean vk_rpmod_sky_sun( vec3_t dir, vec3_t color ) {
	return RP_LightSun( dir, color );
}

static qboolean RP_Sun( const VkRect2D *r, float *pos, float *color ) {
	const float *axis0 = backEnd.viewParms.ori.axis[0], *axis1 = backEnd.viewParms.ori.axis[1], *axis2 = backEnd.viewParms.ori.axis[2];
	vec3_t sunDir, sunLight;
	float m, tanHalf[2], z, nx, ny, fade;

	if ( !r_sunShafts->integer || r_sunShaftsIntensity->value <= 0.0f || !RP_LightSun( sunDir, sunLight ) )
		return qfalse;
	m = MAX( sunLight[0], MAX( sunLight[1], sunLight[2] ) );

	// in the camera's space as the shaders have it: x right, y down, z forward
	z = DotProduct( sunDir, axis0 );
	if ( z <= 0.05f )
		return qfalse;
	RP_TanHalf( tanHalf );
	nx = -DotProduct( sunDir, axis1 ) / ( z * tanHalf[0] );
	ny = -DotProduct( sunDir, axis2 ) / ( z * tanHalf[1] );
	fade = Com_Clamp( 0.0f, 1.0f, ( z - 0.05f ) * 4.0f ) * Com_Clamp( 0.0f, 1.0f, 2.5f - MAX( fabsf( nx ), fabsf( ny ) ) );
	if ( fade <= 0.0f )
		return qfalse;

	pos[0] = r->offset.x + ( nx * 0.5f + 0.5f ) * r->extent.width;
	pos[1] = r->offset.y + ( ny * 0.5f + 0.5f ) * r->extent.height;
	if ( rp.lightIsMoon )
		fade *= RP_MOON_SHAFTS;
	for ( int i = 0; i < 3; i++ )
		color[i] = sunLight[i] / m * r_sunShaftsIntensity->value * fade;
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
		|| ( r_water->integer && r_waterStrength->value > 0.0f ) || r_heatHaze->integer
		|| ( r_wetSurfaces->integer && R_WetZones( NULL, NULL, NULL, 0 ) > 0 )
		|| r_skySharpen->value > 0.0f || r_aerialHaze->value > 0.0f
		|| r_skyScene[0]->string[0]
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
	vec3_t L, f, up, left, hint, fwd, C, sunColor;
	float viewer[16], proj[16], lightMvp[16], A[16], a, b;

	// toward the light: the sky composer's sun when it has one (an admin's, outdoors), the map's
	// with r_shadowSun, else nearly straight down; a sun too low casts down too
	if ( !( ( rp.skyHasSun || rp.skyHasMoon || rp.skyTime >= 0.0f || r_shadowSun->integer ) && RP_LightSun( L, sunColor ) && L[2] > 0.25f ) )
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
	rp.shadowSized = qtrue;
	vk.cmd->last_pipeline = VK_NULL_HANDLE;
	vk_begin_render_pass( rp.shadowPass, rp.shadowFb, qtrue, RP_SHADOW_SIZE, RP_SHADOW_SIZE );
	Com_Memset( &vk.cmd->scissor_rect, 0, sizeof( vk.cmd->scissor_rect ) );
	RB_RenderDrawSurfList( casters, n );
	vk_end_render_pass();

	backEnd.viewParms = saved;
	backEnd.ori = savedOri;
	rp.shadowSized = qfalse;
	vk.renderPassIndex = RENDER_PASS_MAIN;
	vk.cmd->last_pipeline = VK_NULL_HANDLE;
	vk.cmd->depth_range = DEPTH_RANGE_COUNT;
	Com_Memset( &vk.cmd->scissor_rect, 0, sizeof( vk.cmd->scissor_rect ) );
	Com_Memcpy( vk_world.modelview_transform, backEnd.viewParms.world.modelViewMatrix, 64 );
	return qtrue;
}

/*
===============
reflections: which surfaces are shiny, by the material the map's shader gives them (marble, tiles,
metal, glass, ice, plastic, water) or an environment map on it; grass, dirt, rock, carpet, sand and
the rest, and what has no material, never reflect
===============
*/
static qboolean RP_Shiny( const shader_t *shader ) {
	switch ( shader->surfaceFlags & MATERIAL_MASK ) {
	case MATERIAL_MARBLE:
	case MATERIAL_TILES:
	case MATERIAL_SOLIDMETAL:
	case MATERIAL_HOLLOWMETAL:
	case MATERIAL_GLASS:
	case MATERIAL_BPGLASS:
	case MATERIAL_SHATTERGLASS:
	case MATERIAL_ICE:
	case MATERIAL_PLASTIC:
	case MATERIAL_WATER:
		return qtrue;
	default:
		break;
	}
	for ( int i = 0; i < MAX_SHADER_STAGES && shader->stages[i]; i++ ) {
		if ( shader->stages[i]->active && shader->stages[i]->bundle[0].tcGen == TCGEN_ENVIRONMENT_MAPPED )
			return qtrue;
	}
	return qfalse;
}

// the shiny solid surfaces drawn again, their depth alone, from the camera
static qboolean RP_ShinyMask( drawSurf_t *drawSurfs, int numDrawSurfs ) {
	static drawSurf_t shiny[RP_MAX_CASTERS * 2];
	int n = 0;

	for ( int i = 0; i < numDrawSurfs && n < (int)ARRAY_LEN( shiny ); i++ ) {
		int entityNum, fogNum, dlighted;
		shader_t *shader;

		R_DecomposeSort( drawSurfs[i].sort, &entityNum, &shader, &fogNum, &dlighted );
		if ( shader->sort > SS_OPAQUE || shader->isSky || shader->useDistortion || !RP_Shiny( shader ) )
			continue;
		if ( entityNum != REFENTITYNUM_WORLD
			&& ( backEnd.refdef.entities[entityNum].e.renderfx & ( RF_FIRST_PERSON | RF_DEPTHHACK | RF_DISTORTION ) ) )
			continue;
		shiny[n++] = drawSurfs[i];
	}
	if ( !n )
		return qfalse;

	vk.renderPassIndex = RENDER_PASS_SHADOW;	// the shadow map's pipelines: one sample, depth kept
	vk.cmd->last_pipeline = VK_NULL_HANDLE;
	vk_begin_render_pass( rp.shadowPass, rp.maskFb, qtrue, glConfig.vidWidth, glConfig.vidHeight );
	Com_Memset( &vk.cmd->scissor_rect, 0, sizeof( vk.cmd->scissor_rect ) );
	RB_RenderDrawSurfList( shiny, n );
	vk_end_render_pass();

	vk.renderPassIndex = RENDER_PASS_MAIN;
	vk.cmd->last_pipeline = VK_NULL_HANDLE;
	vk.cmd->depth_range = DEPTH_RANGE_COUNT;
	Com_Memset( &vk.cmd->scissor_rect, 0, sizeof( vk.cmd->scissor_rect ) );
	Com_Memcpy( vk_world.modelview_transform, backEnd.viewParms.world.modelViewMatrix, 64 );
	return qtrue;
}

/*
===============
water: the map's water surfaces (by material, or by being water), drawn again, their depth alone,
from the camera; the shadow map's pipelines write depth for see-through surfaces too
===============
*/
static qboolean RP_Water( const shader_t *shader ) {
	return (qboolean)( !shader->isSky && ( ( shader->surfaceFlags & MATERIAL_MASK ) == MATERIAL_WATER || ( shader->contentFlags & CONTENTS_WATER ) ) );
}

static qboolean RP_WaterMask( drawSurf_t *drawSurfs, int numDrawSurfs ) {
	static drawSurf_t water[RP_MAX_CASTERS];
	int n = 0;

	for ( int i = 0; i < numDrawSurfs && n < (int)ARRAY_LEN( water ); i++ ) {
		int entityNum, fogNum, dlighted;
		shader_t *shader;

		R_DecomposeSort( drawSurfs[i].sort, &entityNum, &shader, &fogNum, &dlighted );
		if ( shader->useDistortion || !RP_Water( shader ) )
			continue;
		// the map's own water only: not a decal, an effect's polygons or a model whose shader is water
		// (a splash or puddle laid on the floor took the water's look, the sky mirrored in it)
		if ( entityNum != REFENTITYNUM_WORLD || shader->polygonOffset || *drawSurfs[i].surface == SF_POLY )
			continue;
		water[n++] = drawSurfs[i];
	}
	if ( !n )
		return qfalse;

	vk.renderPassIndex = RENDER_PASS_SHADOW;
	vk.cmd->last_pipeline = VK_NULL_HANDLE;
	vk_begin_render_pass( rp.shadowPass, rp.waterFb, qtrue, glConfig.vidWidth, glConfig.vidHeight );
	Com_Memset( &vk.cmd->scissor_rect, 0, sizeof( vk.cmd->scissor_rect ) );
	RB_RenderDrawSurfList( water, n );
	vk_end_render_pass();

	vk.renderPassIndex = RENDER_PASS_MAIN;
	vk.cmd->last_pipeline = VK_NULL_HANDLE;
	vk.cmd->depth_range = DEPTH_RANGE_COUNT;
	Com_Memset( &vk.cmd->scissor_rect, 0, sizeof( vk.cmd->scissor_rect ) );
	Com_Memcpy( vk_world.modelview_transform, backEnd.viewParms.world.modelViewMatrix, 64 );
	return qtrue;
}

/*
===============
heat haze: the effects that burn (fire, flames, explosions, thrusters, lava, by their shader's name),
as circles on the screen with their distance, the biggest kept
===============
*/
static qboolean RP_Hot( const shader_t *shader ) {
	static const char *words[] = { "fire", "flame", "explo", "burn", "heat", "thrust", "exhaust", "engine", "torch", "lava", "ember", "blaze", "inferno" };
	char name[MAX_QPATH];

	if ( shader->sort <= SS_OPAQUE )
		return qfalse;
	// a mark left on a wall (a blaster's burn, a scorch): a decal, cold, however it's named
	if ( shader->polygonOffset )
		return qfalse;
	Q_strncpyz( name, shader->name, sizeof( name ) );
	Q_strlwr( name );
	if ( strstr( name, "mark" ) || strstr( name, "scorch" ) || strstr( name, "decal" ) )
		return qfalse;
	for ( size_t i = 0; i < ARRAY_LEN( words ); i++ ) {
		if ( strstr( name, words[i] ) )
			return qtrue;
	}
	return qfalse;
}

static void RP_AddHaze( const vec3_t origin, float radius, const VkRect2D *r ) {
	const float *axis0 = backEnd.viewParms.ori.axis[0], *axis1 = backEnd.viewParms.ori.axis[1], *axis2 = backEnd.viewParms.ori.axis[2];
	vec3_t d;
	float z, tanHalf[2], px, py, pr;
	int k;

	VectorSubtract( origin, backEnd.viewParms.ori.origin, d );
	z = DotProduct( d, axis0 );
	if ( z < 16.0f || radius <= 0.0f )
		return;
	RP_TanHalf( tanHalf );
	px = r->offset.x + ( 1.0f + ( -DotProduct( d, axis1 ) / ( z * tanHalf[0] ) ) ) * 0.5f * r->extent.width;
	py = r->offset.y + ( 1.0f + ( -DotProduct( d, axis2 ) / ( z * tanHalf[1] ) ) ) * 0.5f * r->extent.height;
	// the column of hot air is a good deal bigger than the flame
	pr = radius * 2.5f / ( z * tanHalf[1] ) * 0.5f * r->extent.height;
	if ( pr < 4.0f || px + pr < r->offset.x || py + pr < r->offset.y
		|| px - pr > r->offset.x + r->extent.width || py - pr > r->offset.y + r->extent.height )
		return;
	pr = MIN( pr, r->extent.height * 0.5f );

	// one close to another of them is the same fire: the bigger one stays
	for ( k = 0; k < rp.numHaze; k++ ) {
		if ( Square( rp.haze[k][0] - px ) + Square( rp.haze[k][1] - py ) < Square( MAX( pr, rp.haze[k][2] ) * 0.5f ) ) {
			if ( pr > rp.haze[k][2] )
				Vector4Set( rp.haze[k], px, py, pr, z );
			return;
		}
	}
	if ( rp.numHaze < RP_MAX_HAZE ) {
		Vector4Set( rp.haze[rp.numHaze], px, py, pr, z );
		rp.numHaze++;
		return;
	}
	// full: in place of the smallest, if it's bigger
	for ( k = 0; k < RP_MAX_HAZE; k++ ) {
		if ( rp.haze[k][2] < pr ) {
			Vector4Set( rp.haze[k], px, py, pr, z );
			return;
		}
	}
}

static void RP_FindHaze( drawSurf_t *drawSurfs, int numDrawSurfs, const VkRect2D *r ) {
	rp.numHaze = 0;
	for ( int i = 0; i < numDrawSurfs; i++ ) {
		int entityNum, fogNum, dlighted;
		shader_t *shader;

		R_DecomposeSort( drawSurfs[i].sort, &entityNum, &shader, &fogNum, &dlighted );
		if ( !RP_Hot( shader ) )
			continue;
		if ( *drawSurfs[i].surface == SF_POLY ) {
			// an effect's polygon: its middle, and how far its corners reach
			const srfPoly_t *poly = (const srfPoly_t *)drawSurfs[i].surface;
			vec3_t mid;
			float reach = 0.0f;

			if ( poly->numVerts <= 0 )
				continue;
			VectorClear( mid );
			for ( int v = 0; v < poly->numVerts; v++ )
				VectorAdd( mid, poly->verts[v].xyz, mid );
			VectorScale( mid, 1.0f / poly->numVerts, mid );
			for ( int v = 0; v < poly->numVerts; v++ )
				reach = MAX( reach, Distance( mid, poly->verts[v].xyz ) );
			RP_AddHaze( mid, reach, r );
		} else if ( entityNum != REFENTITYNUM_WORLD ) {
			const refEntity_t *e = &backEnd.refdef.entities[entityNum].e;

			if ( e->renderfx & ( RF_FIRST_PERSON | RF_DEPTHHACK ) )
				continue;
			if ( e->reType == RT_SPRITE || e->reType == RT_ORIENTED_QUAD )
				RP_AddHaze( e->origin, e->radius, r );
		}
	}
}

static void RP_SkyScene( void );
static void RP_SkyWeather( void );
static float RP_CloudCover( void );
static void RP_SunMapStep( void );
static qboolean RP_SunMapReady( void );

/*
===============
the sky (the composer's, its clouds and objects, the skybox sharpened) and what's under it (aerial
haze, cloud shadows), drawn after the solid surfaces and before what's see-through: a saber's blade,
effects and glass against the sky stay in front of it. Into the post images from the view, then
copied back into the view (vk_rpmod_after_opaque)
===============
*/
static qboolean RP_SkyPasses( const VkRect2D *rr ) {
	const VkRect2D r = *rr;
	VkDescriptorSet src = rp.sceneSet;
	int next = 0;
	float cover = 0.0f;
	qboolean skyPass, aerialPass;

	if ( rp.skyPipe == VK_NULL_HANDLE || rp.aerialPipe == VK_NULL_HANDLE || !RP_MainView() )
		return qfalse;
	RP_SkyScene();
	RP_SkyWeather();
	cover = RP_CloudCover();
	skyPass = (qboolean)( rp.skyComposed || r_skySharpen->value > 0.0f || cover > 0.0f || rp.numSkyObjects > 0 );
	aerialPass = (qboolean)( r_aerialHaze->value > 0.0f || ( cover > 0.0f && r_cloudShadows->value > 0.0f ) );
	if ( !skyPass && !aerialPass )
		return qfalse;

	if ( skyPass || aerialPass ) {
		// the sky, then what's under it: before reflections and water, which mirror it
		const float *axis0 = backEnd.viewParms.ori.axis[0], *axis1 = backEnd.viewParms.ori.axis[1], *axis2 = backEnd.viewParms.ori.axis[2];
		const float *o = backEnd.viewParms.ori.origin;
		struct {
			float rect[4];
			float a[4];
			float cam[3][4];
			float sun[4];
			float b[4];
			float c[4];
		} p;
		vec3_t sun;

		RP_SkySun( sun );
		RP_RectVec( &r, p.rect );
		RP_TanHalf( p.a );
		p.a[2] = backEnd.viewParms.zFar;
		p.a[3] = backEnd.refdef.time * 0.001f;
		for ( int i = 0; i < 3; i++ ) {
			p.cam[0][i] = -axis1[i];
			p.cam[1][i] = -axis2[i];
			p.cam[2][i] = axis0[i];
			p.cam[i][3] = o[i];
		}
		VectorCopy( sun, p.sun );
		p.b[0] = cover;
		p.b[1] = rp.cloudDrift[0];
		p.b[2] = rp.cloudDrift[1];

		if ( skyPass ) {
			p.sun[3] = rp.skyComposed ? 1.0f : 0.0f;		// the admin's choice, the same for everyone
			p.b[3] = (float)rp.numSkyObjects;
			p.c[0] = r_skySharpen->value;
			p.c[1] = R_IsRaining() ? 1.0f : 0.0f;
			p.c[2] = backEnd.refdef.time < rp.flashUntil ? 1.0f : 0.0f;
			p.c[3] = 1.0f;
			vk_begin_render_pass( rp.postPass, rp.postFb[next], qfalse, glConfig.vidWidth, glConfig.vidHeight );
			RP_Quad3( rp.skyPipe, src, rp.distSet, rp.skySet, &p, sizeof( p ), &r );
			vk_end_render_pass();
			src = rp.postSet[next];
			next ^= 1;
		}
		if ( aerialPass ) {
			if ( cover > 0.0f && r_cloudShadows->value > 0.0f )
				RP_SunMapStep();
			p.sun[3] = r_aerialHaze->value;
			p.b[3] = RP_SunMapReady() ? r_cloudShadows->value : 0.0f;	// none until it's known where the sun reaches
			if ( rp.lightIsMoon )
				p.b[3] *= RP_MOON_CLOUDS;
			p.c[0] = R_IsRaining() ? 1.0f : 0.0f;
			p.c[1] = r_aerialHazeStart->value;
			p.c[2] = p.c[3] = 0.0f;
			vk_begin_render_pass( rp.postPass, rp.postFb[next], qfalse, glConfig.vidWidth, glConfig.vidHeight );
			RP_Quad3( rp.aerialPipe, src, rp.distSet, rp.sunSet, &p, sizeof( p ), &r );
			vk_end_render_pass();
			src = rp.postSet[next];
			next ^= 1;
		}
	}

	rp.skyResult = src;
	return qtrue;
}

void vk_rpmod_after_opaque( drawSurf_t *drawSurfs, int numDrawSurfs, int numAll ) {
	const float zNear = backEnd.viewParms.zNear, zFar = backEnd.viewParms.zFar;
	const qboolean ssao = (qboolean)( r_ssao->integer && r_ssaoIntensity->value > 0.0f && rp.ssaoPipe != VK_NULL_HANDLE );
	float push[4], toLight[16], shadowBias = 0.0f;
	qboolean shadows = qfalse, sky = qfalse;
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
	rp.haveMask = qfalse;
	if ( r_ssr->integer && r_ssrStrength->value > 0.0f && rp.ssrPipe != VK_NULL_HANDLE && rp.maskFb != VK_NULL_HANDLE )
		rp.haveMask = RP_ShinyMask( drawSurfs, numDrawSurfs );
	// water and what burns are see-through: the whole list for them
	rp.haveWater = qfalse;
	if ( r_water->integer && r_waterStrength->value > 0.0f && rp.waterPipe != VK_NULL_HANDLE && rp.waterFb != VK_NULL_HANDLE )
		rp.haveWater = RP_WaterMask( drawSurfs, numAll );
	rp.numHaze = 0;
	if ( r_heatHaze->integer && r_heatHazeStrength->value > 0.0f && rp.hazePipe != VK_NULL_HANDLE )
		RP_FindHaze( drawSurfs, numAll, &r );
	RP_DepthBarrier( qtrue );
	RP_Pass( rp.distPass, rp.distFb, rp.depthPipe, rp.depthSet, VK_NULL_HANDLE, push, sizeof( push ), &r );
	RP_DepthBarrier( qfalse );
	rp.haveDistance = qtrue;		// for the sky passes; again below for the rest of the frame
	sky = RP_SkyPasses( &r );

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
	if ( sky )
		RP_Quad( rp.copyPipe, rp.skyResult, VK_NULL_HANDLE, NULL, 0, &r );	// the view with its sky
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
		p.a[2] = r_shadowStrength->value * ( rp.lightIsMoon ? RP_MOON_SHADOWS : 1.0f );
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
the sky: the composer's scene (r_skyScene) read when it changes, its objects into their table; the
sun's place, the clouds' drift and the lightning, for the sky's passes
===============
*/
static void RP_UploadTable( VkImage image, const void *bytes, size_t size, uint32_t width, uint32_t height );

static const char *rpSkyKinds[] = { "planet", "gas", "moon", "sun", "station", "nebula", "fleet", "ringed" };

static void RP_SkyScene( void ) {
	static float table[RP_MAX_SKYOBJ * 4 * 4];
	static char scene[4 * MAX_CVAR_VALUE_STRING];
	const char *p;
	int changes = 0;

	for ( int i = 0; i < 4; i++ )
		changes += r_skyScene[i]->modificationCount;
	if ( changes == rp.skySceneCount || rp.skyTable == VK_NULL_HANDLE )
		return;
	rp.skySceneCount = changes;
	scene[0] = '\0';
	for ( int i = 0; i < 4; i++ ) {
		Q_strcat( scene, sizeof( scene ), r_skyScene[i]->string );
		Q_strcat( scene, sizeof( scene ), " " );
	}
	rp.numSkyObjects = 0;
	rp.skyTime = rp.skyClouds = rp.skySunYaw = -1.0f;
	rp.skyLightning = rp.skyComposed = rp.skyHasSun = rp.skyHasMoon = qfalse;
	Com_Memset( table, 0, sizeof( table ) );

	p = scene;
	while ( p ) {
		const char *token = COM_ParseExt( &p, qfalse );
		int kind = -1;

		if ( !token[0] ) {
			// the end (the parser leaves NULL there), or a line's end
			if ( !p || !*p )
				break;
			p++;		// past a ';' or a line's end
			continue;
		}
		if ( !Q_stricmp( token, ";" ) )
			continue;
		if ( !Q_stricmp( token, "time" ) ) {
			rp.skyTime = Com_Clamp( 0.0f, 24.0f, atof( COM_ParseExt( &p, qfalse ) ) );
			continue;
		}
		if ( !Q_stricmp( token, "sunyaw" ) ) {
			rp.skySunYaw = AngleNormalize360( atof( COM_ParseExt( &p, qfalse ) ) );
			continue;
		}
		if ( !Q_stricmp( token, "clouds" ) ) {
			rp.skyClouds = Com_Clamp( 0.0f, 1.0f, atof( COM_ParseExt( &p, qfalse ) ) );
			continue;
		}
		if ( !Q_stricmp( token, "sky" ) ) {
			rp.skyComposed = (qboolean)( !Q_stricmp( COM_ParseExt( &p, qfalse ), "composed" ) );
			continue;
		}
		if ( !Q_stricmp( token, "lightning" ) ) {
			rp.skyLightning = (qboolean)( atoi( COM_ParseExt( &p, qfalse ) ) != 0 );
			continue;
		}
		for ( int k = 0; k < (int)ARRAY_LEN( rpSkyKinds ); k++ ) {
			if ( !Q_stricmp( token, rpSkyKinds[k] ) )
				kind = k;
		}
		if ( kind < 0 || rp.numSkyObjects >= RP_MAX_SKYOBJ )
			continue;
		{
			float v[9] = { 0, 20, 5, 1, 1, 1, 0, 20, 0 };	// yaw pitch size r g b seed tilt rings
			float *t = table + rp.numSkyObjects * 16;
			vec3_t angles, dir;

			for ( int k = 0; k < 9; k++ ) {
				const char *num = COM_ParseExt( &p, qfalse );

				if ( !num[0] || !strchr( "-0123456789.", num[0] ) ) {
					if ( num[0] && p )
						p -= strlen( num );		// the next object's word
					break;
				}
				v[k] = atof( num );
			}
			VectorSet( angles, -v[1], v[0], 0.0f );
			AngleVectors( angles, dir, NULL, NULL );
			VectorCopy( dir, t );
			t[3] = DEG2RAD( Com_Clamp( 0.1f, 60.0f, v[2] ) );
			t[4] = Com_Clamp( 0.0f, 4.0f, v[3] );
			t[5] = Com_Clamp( 0.0f, 4.0f, v[4] );
			t[6] = Com_Clamp( 0.0f, 4.0f, v[5] );
			t[7] = (float)kind;
			t[8] = v[6];
			t[9] = DEG2RAD( v[7] );
			t[10] = v[8];
			t[11] = 1.0f;
			if ( !Q_stricmp( rpSkyKinds[kind], "sun" ) && !rp.skyHasSun ) {
				rp.skyHasSun = qtrue;
				VectorCopy( dir, rp.skySunDir );
				VectorCopy( t + 4, rp.skySunColor );
			}
			if ( !Q_stricmp( rpSkyKinds[kind], "moon" ) && !rp.skyHasMoon ) {
				rp.skyHasMoon = qtrue;
				VectorCopy( dir, rp.skyMoonDir );
				VectorCopy( t + 4, rp.skyMoonColor );
			}
			rp.numSkyObjects++;
		}
	}

	RP_UploadTable( rp.skyTable, table, sizeof( table ), RP_MAX_SKYOBJ * 4, 1 );
}

// a table image refilled: the frames drawing with it done first (it changes rarely)
static void RP_UploadTable( VkImage image, const void *bytes, size_t size, uint32_t width, uint32_t height ) {
	VkBufferImageCopy region;
	{
		VkBufferCreateInfo bdesc;
		VkMemoryRequirements req;
		VkMemoryAllocateInfo alloc;
		VkBuffer staging;
		VkDeviceMemory stagingMemory;
		VkCommandBuffer cmd;
		void *data;

		qvkDeviceWaitIdle( vk.device );
		Com_Memset( &bdesc, 0, sizeof( bdesc ) );
		bdesc.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bdesc.size = size;
		bdesc.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		bdesc.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		VK_CHECK( qvkCreateBuffer( vk.device, &bdesc, NULL, &staging ) );
		qvkGetBufferMemoryRequirements( vk.device, staging, &req );
		Com_Memset( &alloc, 0, sizeof( alloc ) );
		alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		alloc.allocationSize = req.size;
		alloc.memoryTypeIndex = vk_find_memory_type( req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT );
		VK_CHECK( qvkAllocateMemory( vk.device, &alloc, NULL, &stagingMemory ) );
		VK_CHECK( qvkBindBufferMemory( vk.device, staging, stagingMemory, 0 ) );
		VK_CHECK( qvkMapMemory( vk.device, stagingMemory, 0, VK_WHOLE_SIZE, 0, &data ) );
		Com_Memcpy( data, bytes, size );
		qvkUnmapMemory( vk.device, stagingMemory );

		Com_Memset( &region, 0, sizeof( region ) );
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.layerCount = 1;
		region.imageExtent.width = width;
		region.imageExtent.height = height;
		region.imageExtent.depth = 1;
		cmd = vk_begin_command_buffer();
		vk_record_image_layout_transition( cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 0 );
		qvkCmdCopyBufferToImage( cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region );
		vk_record_image_layout_transition( cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 0 );
		vk_end_command_buffer( cmd, __func__ );
		qvkDestroyBuffer( vk.device, staging, NULL );
		qvkFreeMemory( vk.device, stagingMemory, NULL );
	}
}

// toward the sun in the world: by the composer's hour (rising on the map's sun's side, high at noon,
// set by 18), else high on the map's sun's side (60 degrees up), else straight up
static void RP_SkySun( vec3_t out ) {
	vec3_t flat;
	qboolean side = qtrue;		// a side chosen: the hour only sets the sun's height there

	if ( rp.skySunYaw >= 0.0f )
		VectorSet( flat, cosf( DEG2RAD( rp.skySunYaw ) ), sinf( DEG2RAD( rp.skySunYaw ) ), 0.0f );
	else if ( rp.skyHasSun && fabsf( rp.skySunDir[0] ) + fabsf( rp.skySunDir[1] ) > 0.01f )
		VectorCopy( rp.skySunDir, flat );
	else {
		side = qfalse;
		if ( VectorLength( tr.sunDirection ) > 0.1f )
			VectorCopy( tr.sunDirection, flat );
		else
			VectorSet( flat, 0.4f, -0.5f, 0.75f );
	}
	if ( rp.skyTime < 0.0f ) {
		// no hour: a plain day, the sun high on the map's own side (a map's sun often sits low, which
		// read as dusk)
		const float len = sqrtf( flat[0] * flat[0] + flat[1] * flat[1] );

		if ( len < 0.01f )
			VectorSet( out, 0.0f, 0.0f, 1.0f );
		else
			VectorSet( out, flat[0] / len * 0.5f, flat[1] / len * 0.5f, 0.866f );
		return;
	}
	{
		const float e = ( rp.skyTime - 6.0f ) / 12.0f * M_PI;		// 0 at six, pi/2 at noon
		const float height = sinf( e ) * 0.92f;
		float az[2] = { flat[0], flat[1] };
		const float len = sqrtf( az[0] * az[0] + az[1] * az[1] );

		if ( len < 0.01f ) {
			az[0] = 1.0f;
			az[1] = 0.0f;
		} else {
			az[0] /= len;
			az[1] /= len;
		}
		// along the sun's own way: from one side at six to the other at eighteen; on the side chosen,
		// rising and setting there
		const float across = side ? fabsf( cosf( e ) ) : cosf( e );

		out[0] = az[0] * across;
		out[1] = az[1] * across;
		out[2] = height;
		VectorNormalize( out );
	}
}

/*
===============
the sun map: the whole map seen from the sun, coarsely, each texel how far the sun's light goes before
it lands on something (the map's own brushes, traced: the sky's brushes let it through). A point lower
than that, along the sun, is under a roof: the clouds' shadows don't reach it, wherever the roof is,
on screen or not. Made a little each frame (a few milliseconds), again when the sun moves; the one
made before shown until the new one's done. Row 0 is its header: where it starts, its texel's size,
its size in texels, and the sun it was made for.
===============
*/
static struct {
	const void	*world;
	vec3_t		L;				// the sun the map in the image was made for, or the one being made for
	vec3_t		f, left, up;
	float		u0, v0, texel, dRef, reach;
	int			w, h, next, msSpent;
	qboolean	building, ready;
	float		data[RP_SUNMAP_N * ( RP_SUNMAP_N + 1 )];
} rpSun;

static qboolean RP_SunMapReady( void ) {
	return rpSun.ready;
}

static void RP_SunMapForget( void ) {
	rpSun.world = NULL;
	rpSun.building = rpSun.ready = qfalse;
}

// the light's axes, as the shader works them out from the sun alone
static void RP_SunAxes( const vec3_t L, vec3_t f, vec3_t left, vec3_t up ) {
	vec3_t hint;

	VectorScale( L, -1.0f, f );
	if ( fabsf( f[2] ) > 0.99f )
		VectorSet( hint, 1.0f, 0.0f, 0.0f );
	else
		VectorSet( hint, 0.0f, 0.0f, 1.0f );
	VectorMA( hint, -DotProduct( hint, f ), f, up );
	VectorNormalize( up );
	CrossProduct( up, f, left );
}

static void RP_SunMapStart( const vec3_t L ) {
	const mnode_t *root = tr.world->nodes;
	float umin = 1e9f, umax = -1e9f, vmin = 1e9f, vmax = -1e9f, size;
	vec3_t c;

	VectorCopy( L, rpSun.L );
	RP_SunAxes( L, rpSun.f, rpSun.left, rpSun.up );
	for ( int i = 0; i < 8; i++ ) {
		vec3_t corner;
		float u, v;

		corner[0] = ( i & 1 ) ? root->maxs[0] : root->mins[0];
		corner[1] = ( i & 2 ) ? root->maxs[1] : root->mins[1];
		corner[2] = ( i & 4 ) ? root->maxs[2] : root->mins[2];
		u = DotProduct( corner, rpSun.left );
		v = DotProduct( corner, rpSun.up );
		umin = MIN( umin, u ); umax = MAX( umax, u );
		vmin = MIN( vmin, v ); vmax = MAX( vmax, v );
	}
	size = MAX( umax - umin, vmax - vmin );
	rpSun.texel = MAX( 32.0f, size / RP_SUNMAP_N );
	rpSun.w = MIN( RP_SUNMAP_N, (int)ceilf( ( umax - umin ) / rpSun.texel ) );
	rpSun.h = MIN( RP_SUNMAP_N, (int)ceilf( ( vmax - vmin ) / rpSun.texel ) );
	rpSun.u0 = umin;
	rpSun.v0 = vmin;
	VectorAdd( root->mins, root->maxs, c );
	VectorScale( c, 0.5f, c );
	rpSun.dRef = DotProduct( c, rpSun.f );
	rpSun.reach = Distance( root->mins, root->maxs ) + 512.0f;
	rpSun.next = rpSun.msSpent = 0;
	rpSun.building = qtrue;
	rpSun.world = tr.world;
}

// one texel: the sun's light traced down through it, past the sky's brushes
static float RP_SunMapTexel( int i, int j ) {
	const float u = rpSun.u0 + ( i + 0.5f ) * rpSun.texel, v = rpSun.v0 + ( j + 0.5f ) * rpSun.texel;
	vec3_t x, start, end;
	trace_t tr2;

	VectorScale( rpSun.left, u, x );
	VectorMA( x, v, rpSun.up, x );
	VectorMA( x, rpSun.dRef, rpSun.f, x );
	VectorMA( x, -rpSun.reach, rpSun.f, start );
	VectorMA( x, rpSun.reach, rpSun.f, end );
	for ( int k = 0; k < 6; k++ ) {
		ri.CM_BoxTrace( &tr2, start, end, NULL, NULL, 0, CONTENTS_SOLID | CONTENTS_TERRAIN, qfalse );
		if ( tr2.fraction >= 1.0f || tr2.allsolid )
			return 1e9f;		// nothing in the way: all of it in the sun
		if ( !( tr2.surfaceFlags & SURF_SKY ) )
			return DotProduct( tr2.endpos, rpSun.f );
		VectorMA( tr2.endpos, 4.0f, rpSun.f, start );	// into the sky's brush, and on through it
	}
	return 1e9f;
}

// a little more of it this frame; the image refilled when it's done
static void RP_SunMapStep( void ) {
	vec3_t L, color;
	int t0;

	if ( !tr.world || !tr.world->nodes || !rp.sunMap || !RP_LightSun( L, color ) || L[2] < 0.05f )
		return;
	if ( rpSun.world != tr.world || ( !rpSun.building && DotProduct( L, rpSun.L ) < 0.99996f )
		|| ( rpSun.building && DotProduct( L, rpSun.L ) < 0.99996f ) ) {
		if ( rpSun.world != tr.world )
			rpSun.ready = qfalse;
		RP_SunMapStart( L );
	}
	if ( !rpSun.building )
		return;
	t0 = ri.Milliseconds();
	while ( rpSun.next < rpSun.w * rpSun.h ) {
		const int i = rpSun.next % rpSun.w, j = rpSun.next / rpSun.w;

		rpSun.data[( j + 1 ) * RP_SUNMAP_N + i] = RP_SunMapTexel( i, j );
		rpSun.next++;
		if ( ( rpSun.next & 31 ) == 0 && ri.Milliseconds() - t0 >= 3 ) {
			rpSun.msSpent += ri.Milliseconds() - t0;
			return;
		}
	}
	rpSun.msSpent += ri.Milliseconds() - t0;
	ri.Printf( PRINT_DEVELOPER, "sun map: %ix%i texels of %g units, %i ms of traces\n", rpSun.w, rpSun.h, rpSun.texel, rpSun.msSpent );
	rpSun.data[0] = rpSun.u0;
	rpSun.data[1] = rpSun.v0;
	rpSun.data[2] = rpSun.texel;
	rpSun.data[3] = (float)rpSun.w;
	rpSun.data[4] = (float)rpSun.h;
	rpSun.data[5] = rpSun.L[0];
	rpSun.data[6] = rpSun.L[1];
	rpSun.data[7] = rpSun.L[2];
	RP_UploadTable( rp.sunMap, rpSun.data, sizeof( rpSun.data ), RP_SUNMAP_N, RP_SUNMAP_N + 1 );
	rpSun.building = qfalse;
	rpSun.ready = qtrue;
}

// the clouds' cover now: the sky composer's (none without it, or hidden by r_clouds 0); more in rain
static float RP_CloudCover( void ) {
	float cover = ( rp.skyClouds > 0.0f && r_clouds->value > 0.0f ) ? rp.skyClouds : 0.0f;

	if ( cover > 0.0f && R_IsRaining() )
		cover = MAX( cover, 0.8f );
	return cover;
}

// the clouds' drift with the weather's wind (a little of their own without it), and the lightning
static void RP_SkyWeather( void ) {
	const int now = backEnd.refdef.time;
	const float dt = ( rp.cloudTime && now > rp.cloudTime && now - rp.cloudTime < 1000 ) ? ( now - rp.cloudTime ) * 0.001f : 0.0f;
	vec3_t wind;

	rp.cloudTime = now;
	if ( !R_GetWindVector( wind ) || VectorLength( wind ) < 1.0f )
		VectorSet( wind, 30.0f, 12.0f, 0.0f );
	rp.cloudDrift[0] -= wind[0] * dt * 2.0f;
	rp.cloudDrift[1] -= wind[1] * dt * 2.0f;

	if ( rp.skyLightning && now >= rp.nextFlash ) {
		rp.flashUntil = now + 80 + ( rand() % 160 );
		rp.nextFlash = now + 3000 + ( rand() % 9000 );
	}
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
	const qboolean smaa = (qboolean)( r_smaa->integer != 0 && rp.smaaPipe[2] != VK_NULL_HANDLE && rp.smaaTableView[0] != VK_NULL_HANDLE );
	const qboolean fxaa = (qboolean)( r_fxaa->integer != 0 && !smaa );
	const qboolean sharpen = (qboolean)( r_sharpen->value > 0.0f );
	const qboolean ssr = (qboolean)( dist && rp.haveMask && r_ssr->integer && r_ssrStrength->value > 0.0f && rp.ssrPipe != VK_NULL_HANDLE
		&& rp.ssrBlurPipe != VK_NULL_HANDLE && rp.smaaFb[0] != VK_NULL_HANDLE );
	const qboolean water = (qboolean)( dist && rp.haveWater && r_water->integer && rp.waterPipe != VK_NULL_HANDLE );
	const qboolean haze = (qboolean)( dist && rp.numHaze > 0 && r_heatHaze->integer && rp.hazePipe != VK_NULL_HANDLE );
	vec3_t wetMins[RP_MAX_WET], wetMaxs[RP_MAX_WET];
	float wetness[RP_MAX_WET];
	const int numWet = ( dist && r_wetSurfaces->integer && r_wetStrength->value > 0.0f && rp.wetPipe != VK_NULL_HANDLE )
		? R_WetZones( wetMins, wetMaxs, wetness, RP_MAX_WET ) : 0;
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
	if ( !shafts && !ssr && !water && !numWet && !haze && !motion && !dof && !graded && !filter && !smaa && !fxaa && !sharpen )
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
		p.up[3] = backEnd.viewParms.zNear;
		// the reflections alone (colour, and how strongly) into the SMAA edges' image
		vk_begin_render_pass( rp.smaaPass, rp.smaaFb[0], qfalse, glConfig.vidWidth, glConfig.vidHeight );
		RP_Quad3( rp.ssrPipe, src, rp.distSet, rp.maskSet, &p, sizeof( p ), &r );
		vk_end_render_pass();
		// then softened at their outlines and laid on the view
		vk_begin_render_pass( rp.postPass, rp.postFb[next], qfalse, glConfig.vidWidth, glConfig.vidHeight );
		RP_Quad3( rp.ssrBlurPipe, src, rp.smaaSet[0], rp.distSet, p.rect, sizeof( p.rect ), &r );
		vk_end_render_pass();
		src = rp.postSet[next];
		next ^= 1;
	}

	// the camera in the world, for water's and the wet ground's: x right, y down, z forward, and where it is
	if ( water || numWet ) {
		const float *axis0 = backEnd.viewParms.ori.axis[0], *axis1 = backEnd.viewParms.ori.axis[1], *axis2 = backEnd.viewParms.ori.axis[2];
		const float *o = backEnd.viewParms.ori.origin;
		const float seconds = backEnd.refdef.time * 0.001f;
		float cam[3][4];

		for ( int i = 0; i < 3; i++ ) {
			cam[0][i] = -axis1[i];
			cam[1][i] = -axis2[i];
			cam[2][i] = axis0[i];
			cam[i][3] = o[i];
		}

		if ( water ) {
			struct {
				float rect[4];
				float a[4];
				float cam[3][4];
				float b[4];
			} p;

			RP_RectVec( &r, p.rect );
			RP_TanHalf( p.a );
			p.a[2] = r_waterStrength->value;
			p.a[3] = backEnd.viewParms.zFar;
			Com_Memcpy( p.cam, cam, sizeof( p.cam ) );
			p.b[0] = seconds;
			p.b[1] = backEnd.viewParms.zNear;
			p.b[2] = r_waterRipples->value;
			p.b[3] = 0.0f;
			vk_begin_render_pass( rp.postPass, rp.postFb[next], qfalse, glConfig.vidWidth, glConfig.vidHeight );
			RP_Quad3( rp.waterPipe, src, rp.distSet, rp.waterSet, &p, sizeof( p ), &r );
			vk_end_render_pass();
			src = rp.postSet[next];
			next ^= 1;
		}

		for ( int w = 0; w < numWet; w++ ) {
			struct {
				float rect[4];
				float a[4];
				float cam[3][4];
				float mins[4];
				float maxs[4];
			} p;

			RP_RectVec( &r, p.rect );
			RP_TanHalf( p.a );
			p.a[2] = wetness[w];
			p.a[3] = backEnd.viewParms.zFar;
			Com_Memcpy( p.cam, cam, sizeof( p.cam ) );
			VectorCopy( wetMins[w], p.mins );
			p.mins[3] = seconds;
			VectorCopy( wetMaxs[w], p.maxs );
			p.maxs[3] = r_wetStrength->value;
			RP_NEXT( rp.wetPipe, rp.distSet, p );
		}
	}

	if ( haze ) {
		// six sources a pass, over what's there
		for ( int first = 0; first < rp.numHaze; first += 6 ) {
			struct {
				float rect[4];
				float a[4];
				float src[6][4];
			} p;
			const int count = MIN( 6, rp.numHaze - first );

			Com_Memset( &p, 0, sizeof( p ) );
			RP_RectVec( &r, p.rect );
			p.a[0] = backEnd.refdef.time * 0.001f;
			p.a[1] = r_heatHazeStrength->value * glConfig.vidHeight / 480.0f;
			p.a[2] = (float)count;
			for ( int k = 0; k < count; k++ )
				Vector4Copy( rp.haze[first + k], p.src[k] );
			RP_NEXT( rp.hazePipe, rp.distSet, p );
		}
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

	if ( smaa ) {
		// the edges, how much each takes from its neighbours, then the view blended by that
		struct {
			float metrics[4];
		} p;

		p.metrics[0] = 1.0f / glConfig.vidWidth;
		p.metrics[1] = 1.0f / glConfig.vidHeight;
		p.metrics[2] = (float)glConfig.vidWidth;
		p.metrics[3] = (float)glConfig.vidHeight;
		RP_Pass( rp.smaaPass, rp.smaaFb[0], rp.smaaPipe[0], src, VK_NULL_HANDLE, &p, sizeof( p ), &r );
		vk_begin_render_pass( rp.smaaPass, rp.smaaFb[1], qfalse, glConfig.vidWidth, glConfig.vidHeight );
		RP_Quad3( rp.smaaPipe[1], rp.smaaSet[0], rp.smaaTableSet[0], rp.smaaTableSet[1], &p, sizeof( p ), &r );
		vk_end_render_pass();
		RP_NEXT( rp.smaaPipe[2], rp.smaaSet[1], p );
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

/*
===============
vk_rpmod_fsr

With FSR, the view drawn smaller is scaled up to the window and sharpened. After the frame's last
world view, at its first 2D (vk_rpmod_fsr_window, from vk_set_2d), so the HUD, the console and the
menus are drawn on it at the window's own size, sharp; else at the frame's end, before the gamma pass
(vk_rpmod_fsr). The image set the gamma pass shows, the picture's own without FSR
===============
*/
// the picture FSR made, for screenshots: the window's size, as it's seen; NULL without FSR
VkImage vk_rpmod_fsr_image( uint32_t *width, uint32_t *height ) {
	if ( !rp.ready || rp.fsrFb[1] == VK_NULL_HANDLE || !r_fsr->integer )
		return VK_NULL_HANDLE;
	*width = gls.windowWidth;
	*height = gls.windowHeight;
	return rp.fsrImage[1];
}

static qboolean RP_FsrReady( void ) {
	return (qboolean)( rp.ready && rp.setsAllocated && rp.fsrFb[0] != VK_NULL_HANDLE && rp.fsrPipe[0] != VK_NULL_HANDLE && r_fsr->integer );
}

// the view so far, scaled up and sharpened into fsrImage[1]; outside any render pass
static void RP_FsrUpscale( void ) {
	struct {
		float a[4];
		float b[4];
	} p;
	VkRect2D r;

	p.a[0] = p.a[2] = (float)glConfig.vidWidth;
	p.a[1] = p.a[3] = (float)glConfig.vidHeight;
	p.b[0] = (float)gls.windowWidth;
	p.b[1] = (float)gls.windowHeight;
	p.b[2] = 0.2f;		// AMD's suggested sharpening, in stops from the most
	p.b[3] = 0.0f;
	r.offset.x = r.offset.y = 0;
	r.extent.width = gls.windowWidth;
	r.extent.height = gls.windowHeight;

	vk_begin_render_pass( rp.postPass, rp.fsrFb[0], qfalse, gls.windowWidth, gls.windowHeight );
	RP_Quad( rp.fsrPipe[0], rp.sceneSet, VK_NULL_HANDLE, &p, sizeof( p ), &r );
	vk_end_render_pass();
	vk_begin_render_pass( rp.postPass, rp.fsrFb[1], qfalse, gls.windowWidth, gls.windowHeight );
	RP_Quad( rp.fsrPipe[1], rp.fsrSet[0], VK_NULL_HANDLE, &p, sizeof( p ), &r );
	vk_end_render_pass();
}

// the frame's start: drawn at the view's size again
void vk_rpmod_fsr_begin_frame( void ) {
	rp.fsrWindow = qfalse;
}

// the rest of the frame drawn at the window's size (no dynamic glow or refraction then: their passes
// are the view's size)
qboolean vk_rpmod_fsr_drawing_window( void ) {
	return rp.fsrWindow;
}

// on the way to 2D: once the frame's world views are all drawn, the view scaled up now, and what
// follows drawn over it at the window's size
void vk_rpmod_fsr_window( void ) {
	if ( rp.fsrWindow || rp.fsrWindowFb == VK_NULL_HANDLE || !RP_FsrReady() || !vk.fboActive )
		return;
	if ( backEnd.rpWorldViews > 0 || vk.renderPassIndex != RENDER_PASS_MAIN )
		return;

	// bloom first, on the view at its own size, and not again over the window's picture
	if ( vk.bloomActive )
		vk_bloom();
	backEnd.doneBloom = qtrue;

	vk_end_render_pass();
	RP_FsrUpscale();

	vk.renderPassIndex = RENDER_PASS_MAIN;
	vk.renderWidth = gls.windowWidth;
	vk.renderHeight = gls.windowHeight;
	vk.renderScaleX = (float)gls.windowWidth / (float)glConfig.vidWidth;
	vk.renderScaleY = (float)gls.windowHeight / (float)glConfig.vidHeight;
	vk_begin_render_pass( rp.fsrWindowPass, rp.fsrWindowFb, qtrue, vk.renderWidth, vk.renderHeight );
	RP_Restore();
	rp.fsrWindow = qtrue;
}

VkDescriptorSet vk_rpmod_fsr( void ) {
	if ( rp.fsrWindow ) {
		rp.fsrWindow = qfalse;
		return rp.fsrSet[1];
	}
	if ( !RP_FsrReady() )
		return vk.color_descriptor;
	RP_FsrUpscale();
	return rp.fsrSet[1];
}
