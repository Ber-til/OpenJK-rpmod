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

// cl_shadermanager.cpp -- finds the shaders of what you look at, and remaps them
//
// "shadermanager crosshair" names the shader under the crosshair. "shadermanager"
// opens a free camera: click something to list its shaders, pick a replacement
// from every shader the game can find, see it on this screen first, then send
// RPMod's "rpshader remap" so the server applies it for everyone.
//
// Collision traces only know brushes and surface flags, so the drawn surfaces are
// traced instead: the map's own surfaces read out of its .bsp (planar, patches,
// triangle soups such as baked misc_models), and the entities cgame hands the
// renderer each frame (md3s and doors/movers by their geometry, Ghoul2 models
// through the Ghoul2 collision code). The shader is worked out the way the
// renderer picks it: custom shader, then skin, then the model's own.
//
// The model, skin and remap names come from watching cgame's register and remap
// calls, since the renderer can't turn those handles back into names.

#include <algorithm>
#include <map>
#include <vector>
#include <string>
#include "client.h"
#include "qcommon/cm_public.h"
#include "ghoul2/G2.h"

extern IHeapAllocator *G2VertSpaceClient;

#define SM_TOGGLE_CMD		"shadermanager"
#define SM_TRACE_DIST		16384.0f
#define SM_PATCH_STEPS		6		// samples per 3x3 patch piece, each way
#define SM_MAX_ENTITIES		2048
#define SM_MAX_HANDLES		8192
#define SM_SELF_RADIUS		48.0f	// entities this close to the player are taken as the player
#define SM_TEXT_SCALE		0.8f
#define SM_FLY_SPEED		400.0f
#define SM_REPLY_MS			3000	// server prints this soon after a command are shown as its reply
#define SM_MAX_HISTORY		64
#define SM_MAX_OUTLINE		2048	// triangles of a surface worth outlining
#define SM_FIELD_LEN		64

// the manager's panel, docked on the right; the view shrinks to the left of it
#define SM_PANEL_X			368.0f
#define SM_PANEL_W			( SCREEN_WIDTH - SM_PANEL_X - 8.0f )
#define SM_ROW_H			13.0f
#define SM_CTRL_H			15.0f

typedef enum {
	SM_HIT_NONE,
	SM_HIT_WORLD,
	SM_HIT_BRUSH,		// a door, mover or other brush entity
	SM_HIT_MODEL,		// an md3
	SM_HIT_GHOUL2
} smHitKind_t;

typedef struct smHit_s {
	smHitKind_t	kind;
	float		dist;
	vec3_t		pos;
	char		shader[MAX_QPATH];
	char		model[MAX_QPATH];		// the model, "*n" for brush entities
	char		surface[MAX_QPATH];		// the model's surface, "" for the world
	int			surf;					// map surface hit, world and brush entities, -1 otherwise
	refEntity_t	ent;					// the entity hit, all but the world
} smHit_t;

typedef enum {
	SM_OFF,
	SM_CAMERA,		// flying around, the crosshair picks
	SM_EDIT			// the panel is open on what was picked
} smState_t;

typedef enum {
	SM_TAB_OBJECT,
	SM_TAB_REMAPS
} smTab_t;

typedef enum {
	SM_FILTER_ALL,
	SM_FILTER_TEXTURES,
	SM_FILTER_MODELS,
	SM_FILTER_GFX
} smFilter_t;

typedef enum {
	SM_F_NONE,			// typing goes to the search box
	SM_F_SEARCH,
	SM_F_OFFSET,
	SM_NUM_FIELDS
} smField_t;

typedef struct smList_s {
	int			sel, scroll;
} smList_t;

// the map's drawn geometry
typedef struct smSurf_s {
	int			shader;				// into bspShaders
	int			firstTri, numTris;	// into tris, three vertex numbers each
	vec3_t		mins, maxs;
} smSurf_t;

typedef struct smBModel_s {
	int			firstSurf, numSurfs;
	vec3_t		mins, maxs;
} smBModel_t;

// an md3, kept for tracing
typedef struct smMd3Surf_s {
	std::string	name;
	std::vector<std::string> shaders;
	int			numVerts;
	std::vector<int> tris;
	std::vector<float> xyz;			// numFrames * numVerts * 3
} smMd3Surf_t;

typedef struct smMd3_s {
	bool		loaded, valid;
	int			numFrames;
	std::vector<smMd3Surf_t> surfs;
} smMd3_t;

// a .skin: surface -> shader
typedef struct smSkin_s {
	bool		loaded;
	std::vector<std::string> surfaces, shaders;
} smSkin_t;

// a .glm: surface number -> name and shader
typedef struct smGlm_s {
	std::vector<std::string> surfaces, shaders;
} smGlm_t;

static struct {
	qboolean	initialized;
	int			font;

	// the map
	char		mapName[MAX_QPATH];	// "" until read
	qboolean	mapLoaded;
	std::vector<std::string> bspShaders;
	std::vector<float> verts;		// xyz
	std::vector<int> tris;
	std::vector<smSurf_t> surfs;
	std::vector<smBModel_t> bmodels;

	// what cgame registered
	std::string	modelNames[SM_MAX_HANDLES];
	std::string	skinNames[SM_MAX_HANDLES];
	smMd3_t		md3s[SM_MAX_HANDLES];
	smSkin_t	skins[SM_MAX_HANDLES];
	std::vector<std::string> glmNames;
	std::vector<smGlm_t> glms;
	std::vector<std::string> remapFrom, remapTo;

	// this frame's main view and the entities in it
	qboolean	haveView;
	int			viewFrame;
	vec3_t		viewOrg;
	matrix3_t	viewAxis;
	float		viewFovX, viewFovY;
	float		viewX, viewW;		// the part of the virtual screen it covers
	refEntity_t	pending[SM_MAX_ENTITIES];
	int			numPending;
	refEntity_t	entities[SM_MAX_ENTITIES];
	int			numEntities;

	// the last thing found, refreshed once a frame
	int			hitFrame;
	qboolean	hitValid;			// there was a ray to trace
	smHit_t		hit;

	// the manager
	smState_t	state;
	smTab_t		tab;
	int			renderedFrame;
	vec3_t		camOrg, camAng;
	qboolean	held[MAX_KEYS];
	qboolean	looking;			// right mouse held over the panel: the camera moves
	int			lastFrameTime;
	float		cursorX, cursorY;
	qboolean	click;
	float		clickX, clickY;
	int			wheel;
	smField_t	focus;
	char		fields[SM_NUM_FIELDS][SM_FIELD_LEN];

	// what was clicked
	qboolean	haveSel;
	smHitKind_t	selKind;
	char		selWhat[MAX_QPATH * 2];
	std::vector<std::string> selShaders;
	smList_t	selList;
	int			selSurf;			// map surface to outline, -1 for none
	char		selBModel[MAX_QPATH];	// "*n" when that surface belongs to a brush entity

	// every shader the game can find
	qboolean	indexed;
	std::vector<std::string> shaderNames;
	std::map<std::string, qhandle_t> previews;	// 2D handles, by lowercase name
	std::vector<int> shaderView;
	smFilter_t	filter;
	smList_t	shaderList;
	qboolean	showPreview;
	qboolean	livePreview;
	char		liveFrom[MAX_QPATH], liveTo[MAX_QPATH];	// remapped on this screen only

	// remaps tab
	smList_t	remapList;

	// what was sent, for undo: the shader and what it was remapped to before
	std::vector<std::string> undoShader, undoPrevious;

	// messages
	char		status[MAX_STRING_CHARS];
	int			sentTime;
	char		reply[MAX_STRING_CHARS];
	int			replyTime;
} sm;

static cvar_t *cl_shaderCrosshair;

static vec4_t smWhite	= { 1.0f, 1.0f, 1.0f, 1.0f };
static vec4_t smPanel	= { 0.0f, 0.0f, 0.0f, 0.55f };

/*
===============================================================================

NAMES

===============================================================================
*/

// shader names are compared without extension, as the renderer does
static void SM_ShaderKey( const char *name, char *out, int size ) {
	COM_StripExtension( name, out, size );
	Q_strlwr( out );
	for ( char *p = out; *p; p++ ) {
		if ( *p == '\\' )
			*p = '/';
	}
}

static const char *SM_RemappedTo( const char *shader ) {
	char key[MAX_QPATH];

	SM_ShaderKey( shader, key, sizeof( key ) );
	for ( size_t i = 0; i < sm.remapFrom.size(); i++ ) {
		if ( sm.remapFrom[i] == key )
			return sm.remapTo[i].c_str();
	}
	return NULL;
}

static void SM_NoteRemap( const char *oldShader, const char *newShader ) {
	char from[MAX_QPATH], to[MAX_QPATH];

	if ( !oldShader || !newShader )
		return;
	SM_ShaderKey( oldShader, from, sizeof( from ) );
	SM_ShaderKey( newShader, to, sizeof( to ) );
	for ( size_t i = 0; i < sm.remapFrom.size(); i++ ) {
		if ( sm.remapFrom[i] == from ) {
			sm.remapFrom.erase( sm.remapFrom.begin() + i );
			sm.remapTo.erase( sm.remapTo.begin() + i );
			break;
		}
	}
	// a shader remapped to itself is back to normal
	if ( strcmp( from, to ) ) {
		sm.remapFrom.push_back( from );
		sm.remapTo.push_back( to );
	}
}

qhandle_t CL_ShaderManager_RegisterModel( const char *name ) {
	qhandle_t h = re->RegisterModel( name );

	if ( h > 0 && h < SM_MAX_HANDLES && name ) {
		if ( Q_stricmp( sm.modelNames[h].c_str(), name ) ) {
			sm.modelNames[h] = name;
			sm.md3s[h] = smMd3_t();
		}
	}
	return h;
}

qhandle_t CL_ShaderManager_RegisterSkin( const char *name ) {
	qhandle_t h = re->RegisterSkin( name );

	if ( h > 0 && h < SM_MAX_HANDLES && name ) {
		if ( Q_stricmp( sm.skinNames[h].c_str(), name ) ) {
			sm.skinNames[h] = name;
			sm.skins[h] = smSkin_t();
		}
	}
	return h;
}

void CL_ShaderManager_RemapShader( const char *oldShader, const char *newShader, const char *timeOffset ) {
	re->RemapShader( oldShader, newShader, timeOffset );
	SM_NoteRemap( oldShader, newShader );
}

static const char *SM_ShaderName( qhandle_t h ) {
	const char *name;

	if ( h <= 0 )
		return NULL;
	name = re->ShaderNameFromIndex( h );
	return name && name[0] ? name : NULL;
}

/*
===============================================================================

SKINS AND MODELS

===============================================================================
*/

// one .skin file's "surface,shader" lines, the way the renderer reads them
static void SM_ParseSkinFile( const char *path, smSkin_t *skin ) {
	char *buf, *p, *line, *comma;

	if ( FS_ReadFile( path, (void **)&buf ) <= 0 || !buf )
		return;
	for ( p = buf; *p; ) {
		line = p;
		while ( *p && *p != '\n' && *p != '\r' )
			p++;
		if ( *p )
			*p++ = '\0';
		comma = strchr( line, ',' );
		if ( !comma )
			continue;
		*comma = '\0';
		char surf[MAX_QPATH], shader[MAX_QPATH];
		Q_strncpyz( surf, line, sizeof( surf ) );
		Q_strncpyz( shader, comma + 1, sizeof( shader ) );
		Q_strlwr( surf );
		// trim
		for ( char *s : { surf, shader } ) {
			int len = strlen( s ), start = 0;

			while ( len && ( s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '"' ) )
				s[--len] = '\0';
			while ( s[start] == ' ' || s[start] == '\t' || s[start] == '"' )
				start++;
			memmove( s, s + start, len - start + 1 );
		}
		if ( !surf[0] || !shader[0] || !strncmp( surf, "tag_", 4 ) )
			continue;
		skin->surfaces.push_back( surf );
		skin->shaders.push_back( shader );
	}
	FS_FreeFile( buf );
}

// the file a skin handle was registered from, NULL if cgame didn't register it
const char *CL_ShaderManager_SkinName( qhandle_t h ) {
	if ( h <= 0 || h >= SM_MAX_HANDLES || sm.skinNames[h].empty() )
		return NULL;
	return sm.skinNames[h].c_str();
}

static smSkin_t *SM_GetSkin( qhandle_t h ) {
	smSkin_t *skin;
	const char *name;

	if ( h <= 0 || h >= SM_MAX_HANDLES || sm.skinNames[h].empty() )
		return NULL;
	skin = &sm.skins[h];
	if ( skin->loaded )
		return skin;
	skin->loaded = true;

	// "models/players/x/|head|torso|lower" is three files, as RE_SplitSkins does it
	name = sm.skinNames[h].c_str();
	if ( strchr( name, '|' ) ) {
		char base[MAX_QPATH], *part, *next;

		Q_strncpyz( base, name, sizeof( base ) );
		part = strchr( base, '|' );
		*part++ = '\0';
		while ( part && *part ) {
			next = strchr( part, '|' );
			if ( next )
				*next++ = '\0';
			SM_ParseSkinFile( va( "%s%s.skin", base, part ), skin );
			part = next;
		}
	} else {
		SM_ParseSkinFile( name, skin );
	}
	return skin;
}

static const char *SM_SkinShader( qhandle_t h, const char *surface ) {
	smSkin_t *skin = SM_GetSkin( h );

	if ( !skin )
		return NULL;
	for ( size_t i = 0; i < skin->surfaces.size(); i++ ) {
		if ( !Q_stricmp( skin->surfaces[i].c_str(), surface ) )
			return skin->shaders[i].c_str();
	}
	return NULL;
}

static smMd3_t *SM_GetMd3( qhandle_t h ) {
	smMd3_t *m;
	byte *buf;
	int len;
	const md3Header_t *header;
	const char *name;

	if ( h <= 0 || h >= SM_MAX_HANDLES || sm.modelNames[h].empty() )
		return NULL;
	m = &sm.md3s[h];
	if ( m->loaded )
		return m->valid ? m : NULL;
	m->loaded = true;

	name = sm.modelNames[h].c_str();
	len = FS_ReadFile( name, (void **)&buf );
	if ( len <= 0 || !buf )
		return NULL;
	header = (const md3Header_t *)buf;
	if ( len >= (int)sizeof( *header ) && LittleLong( header->ident ) == MD3_IDENT ) {
		int numFrames = LittleLong( header->numFrames ), numSurfaces = LittleLong( header->numSurfaces );
		int ofs = LittleLong( header->ofsSurfaces );
		bool ok = numFrames > 0 && numSurfaces >= 0;

		m->numFrames = numFrames;
		for ( int i = 0; i < numSurfaces && ok; i++ ) {
			const md3Surface_t *s;
			smMd3Surf_t out;
			int numVerts, numTris, numShaders;

			if ( ofs < 0 || ofs > len - (int)sizeof( md3Surface_t ) ) {
				ok = false;
				break;
			}
			s = (const md3Surface_t *)( buf + ofs );
			numVerts = LittleLong( s->numVerts );
			numTris = LittleLong( s->numTriangles );
			numShaders = LittleLong( s->numShaders );
			if ( numVerts < 0 || numTris < 0 || numShaders < 0
				|| LittleLong( s->numFrames ) != numFrames
				|| ofs + LittleLong( s->ofsTriangles ) + numTris * (int)sizeof( md3Triangle_t ) > len
				|| ofs + LittleLong( s->ofsShaders ) + numShaders * (int)sizeof( md3Shader_t ) > len
				|| (int64_t)ofs + LittleLong( s->ofsXyzNormals ) + (int64_t)numVerts * numFrames * (int)sizeof( md3XyzNormal_t ) > len ) {
				ok = false;
				break;
			}

			out.name = s->name;
			Q_strlwr( &out.name[0] );
			out.numVerts = numVerts;
			for ( int j = 0; j < numShaders; j++ ) {
				const md3Shader_t *sh = (const md3Shader_t *)( buf + ofs + LittleLong( s->ofsShaders ) ) + j;
				char shader[MAX_QPATH];

				Q_strncpyz( shader, sh->name, sizeof( shader ) );
				out.shaders.push_back( shader );
			}
			const md3Triangle_t *tri = (const md3Triangle_t *)( buf + ofs + LittleLong( s->ofsTriangles ) );
			for ( int j = 0; j < numTris; j++ ) {
				for ( int k = 0; k < 3; k++ ) {
					int v = LittleLong( tri[j].indexes[k] );
					out.tris.push_back( v >= 0 && v < numVerts ? v : 0 );
				}
			}
			const md3XyzNormal_t *xyz = (const md3XyzNormal_t *)( buf + ofs + LittleLong( s->ofsXyzNormals ) );
			out.xyz.resize( (size_t)numVerts * numFrames * 3 );
			for ( int j = 0; j < numVerts * numFrames; j++ ) {
				for ( int k = 0; k < 3; k++ )
					out.xyz[j * 3 + k] = LittleShort( xyz[j].xyz[k] ) * (float)MD3_XYZ_SCALE;
			}
			m->surfs.push_back( out );
			ofs += LittleLong( s->ofsEnd );
		}
		m->valid = ok;
	}
	FS_FreeFile( buf );
	return m->valid ? m : NULL;
}

// a .glm's surface names and shaders, by surface number
static const smGlm_t *SM_GetGlm( const char *name ) {
	smGlm_t glm;
	byte *buf;
	int len;
	const mdxmHeader_t *header;

	for ( size_t i = 0; i < sm.glmNames.size(); i++ ) {
		if ( !Q_stricmp( sm.glmNames[i].c_str(), name ) )
			return &sm.glms[i];
	}

	len = FS_ReadFile( name, (void **)&buf );
	if ( len > 0 && buf ) {
		header = (const mdxmHeader_t *)buf;
		if ( len >= (int)sizeof( *header ) && LittleLong( header->ident ) == MDXM_IDENT ) {
			int numSurfaces = LittleLong( header->numSurfaces );
			int ofsTable = (int)sizeof( mdxmHeader_t );

			// the hierarchy offsets follow the header and count from there, as G2_API.cpp reads them
			for ( int i = 0; i < numSurfaces; i++ ) {
				const mdxmSurfHierarchy_t *s;
				int at;

				if ( ofsTable + ( i + 1 ) * 4 > len )
					break;
				at = ofsTable + LittleLong( ( (const int *)( buf + ofsTable ) )[i] );
				if ( at < 0 || at > len - (int)sizeof( mdxmSurfHierarchy_t ) )
					break;
				s = (const mdxmSurfHierarchy_t *)( buf + at );
				char surf[MAX_QPATH], shader[MAX_QPATH];
				Q_strncpyz( surf, s->name, sizeof( surf ) );
				Q_strncpyz( shader, s->shader, sizeof( shader ) );
				Q_strlwr( surf );
				glm.surfaces.push_back( surf );
				glm.shaders.push_back( shader );
			}
		}
		FS_FreeFile( buf );
	}
	sm.glmNames.push_back( name );
	sm.glms.push_back( glm );
	return &sm.glms.back();
}

/*
===============================================================================

THE MAP

===============================================================================
*/

static void SM_FreeMap( void ) {
	sm.mapName[0] = '\0';
	sm.mapLoaded = qfalse;
	std::vector<std::string>().swap( sm.bspShaders );
	std::vector<float>().swap( sm.verts );
	std::vector<int>().swap( sm.tris );
	std::vector<smSurf_t>().swap( sm.surfs );
	std::vector<smBModel_t>().swap( sm.bmodels );
}

static int SM_AddVert( const vec3_t v ) {
	sm.verts.push_back( v[0] );
	sm.verts.push_back( v[1] );
	sm.verts.push_back( v[2] );
	return (int)( sm.verts.size() / 3 ) - 1;
}

static void SM_SurfBounds( smSurf_t *s ) {
	ClearBounds( s->mins, s->maxs );
	for ( int i = s->firstTri * 3; i < ( s->firstTri + s->numTris ) * 3; i++ )
		AddPointToBounds( &sm.verts[sm.tris[i] * 3], s->mins, s->maxs );
}

// a patch's surface, sampled from its 3x3 bezier pieces
static void SM_AddPatch( const mapVert_t *verts, int width, int height, smSurf_t *s ) {
	for ( int py = 0; py + 2 < height; py += 2 ) {
		for ( int px = 0; px + 2 < width; px += 2 ) {
			int base = (int)( sm.verts.size() / 3 );

			for ( int j = 0; j <= SM_PATCH_STEPS; j++ ) {
				float v = j / (float)SM_PATCH_STEPS, bv[3] = { ( 1 - v ) * ( 1 - v ), 2 * v * ( 1 - v ), v * v };

				for ( int i = 0; i <= SM_PATCH_STEPS; i++ ) {
					float u = i / (float)SM_PATCH_STEPS, bu[3] = { ( 1 - u ) * ( 1 - u ), 2 * u * ( 1 - u ), u * u };
					vec3_t p = { 0, 0, 0 };

					for ( int b = 0; b < 3; b++ ) {
						for ( int a = 0; a < 3; a++ )
							VectorMA( p, bu[a] * bv[b], verts[( py + b ) * width + px + a].xyz, p );
					}
					SM_AddVert( p );
				}
			}
			for ( int j = 0; j < SM_PATCH_STEPS; j++ ) {
				for ( int i = 0; i < SM_PATCH_STEPS; i++ ) {
					int a = base + j * ( SM_PATCH_STEPS + 1 ) + i, b = a + 1, c = a + SM_PATCH_STEPS + 1, d = c + 1;

					sm.tris.push_back( a ); sm.tris.push_back( b ); sm.tris.push_back( c );
					sm.tris.push_back( b ); sm.tris.push_back( d ); sm.tris.push_back( c );
					s->numTris += 2;
				}
			}
		}
	}
}

static qboolean SM_LumpOk( const dheader_t *h, int lump, int size, int len ) {
	int ofs = LittleLong( h->lumps[lump].fileofs ), n = LittleLong( h->lumps[lump].filelen );

	return (qboolean)( ofs >= 0 && n >= 0 && ofs <= len - n && n % size == 0 );
}

// reads the drawn surfaces out of the map's .bsp
static void SM_LoadMap( const char *mapName ) {
	char path[MAX_QPATH];
	byte *buf;
	int len;
	const dheader_t *h;

	SM_FreeMap();
	Q_strncpyz( sm.mapName, mapName, sizeof( sm.mapName ) );
	sm.mapLoaded = qtrue;	// even if it fails, don't read it every frame

	Com_sprintf( path, sizeof( path ), "maps/%s.bsp", mapName );
	len = FS_ReadFile( path, (void **)&buf );
	if ( len <= 0 || !buf )
		return;
	h = (const dheader_t *)buf;
	if ( len < (int)sizeof( *h ) || LittleLong( h->ident ) != BSP_IDENT || LittleLong( h->version ) != BSP_VERSION
		|| !SM_LumpOk( h, LUMP_SHADERS, sizeof( dshader_t ), len ) || !SM_LumpOk( h, LUMP_MODELS, sizeof( dmodel_t ), len )
		|| !SM_LumpOk( h, LUMP_DRAWVERTS, sizeof( mapVert_t ), len ) || !SM_LumpOk( h, LUMP_DRAWINDEXES, sizeof( int ), len )
		|| !SM_LumpOk( h, LUMP_SURFACES, sizeof( dsurface_t ), len ) ) {
		Com_Printf( S_COLOR_YELLOW "Shader manager: can't read %s\n", path );
		FS_FreeFile( buf );
		return;
	}

	const dshader_t *shaders = (const dshader_t *)( buf + LittleLong( h->lumps[LUMP_SHADERS].fileofs ) );
	const dmodel_t *models = (const dmodel_t *)( buf + LittleLong( h->lumps[LUMP_MODELS].fileofs ) );
	const mapVert_t *verts = (const mapVert_t *)( buf + LittleLong( h->lumps[LUMP_DRAWVERTS].fileofs ) );
	const int *indexes = (const int *)( buf + LittleLong( h->lumps[LUMP_DRAWINDEXES].fileofs ) );
	const dsurface_t *surfs = (const dsurface_t *)( buf + LittleLong( h->lumps[LUMP_SURFACES].fileofs ) );
	int numShaders = LittleLong( h->lumps[LUMP_SHADERS].filelen ) / sizeof( dshader_t );
	int numModels = LittleLong( h->lumps[LUMP_MODELS].filelen ) / sizeof( dmodel_t );
	int numVerts = LittleLong( h->lumps[LUMP_DRAWVERTS].filelen ) / sizeof( mapVert_t );
	int numIndexes = LittleLong( h->lumps[LUMP_DRAWINDEXES].filelen ) / sizeof( int );
	int numSurfs = LittleLong( h->lumps[LUMP_SURFACES].filelen ) / sizeof( dsurface_t );

	for ( int i = 0; i < numShaders; i++ ) {
		char name[MAX_QPATH];

		Q_strncpyz( name, shaders[i].shader, sizeof( name ) );
		sm.bspShaders.push_back( name );
	}

	// every surface belongs to one model: 0 is the world, the rest are brush entities
	for ( int m = 0; m < numModels; m++ ) {
		smBModel_t bm;
		int first = LittleLong( models[m].firstSurface ), count = LittleLong( models[m].numSurfaces );

		bm.firstSurf = (int)sm.surfs.size();
		bm.numSurfs = 0;
		for ( int k = 0; k < 3; k++ ) {
			bm.mins[k] = LittleFloat( models[m].mins[k] );
			bm.maxs[k] = LittleFloat( models[m].maxs[k] );
		}
		for ( int i = first; i < first + count && i >= 0 && i < numSurfs; i++ ) {
			const dsurface_t *in = &surfs[i];
			int type = LittleLong( in->surfaceType ), shader = LittleLong( in->shaderNum );
			int fv = LittleLong( in->firstVert ), nv = LittleLong( in->numVerts );
			int fi = LittleLong( in->firstIndex ), ni = LittleLong( in->numIndexes );
			smSurf_t s;

			if ( shader < 0 || shader >= numShaders || fv < 0 || nv <= 0 || fv > numVerts - nv )
				continue;
			s.shader = shader;
			s.firstTri = (int)( sm.tris.size() / 3 );
			s.numTris = 0;

			if ( type == MST_PLANAR || type == MST_TRIANGLE_SOUP ) {
				int base = (int)( sm.verts.size() / 3 );

				if ( fi < 0 || ni < 3 || fi > numIndexes - ni )
					continue;
				for ( int v = 0; v < nv; v++ ) {
					vec3_t p;

					for ( int k = 0; k < 3; k++ )
						p[k] = LittleFloat( verts[fv + v].xyz[k] );
					SM_AddVert( p );
				}
				for ( int t = 0; t + 2 < ni; t += 3 ) {
					int a = LittleLong( indexes[fi + t] ), b = LittleLong( indexes[fi + t + 1] ), c = LittleLong( indexes[fi + t + 2] );

					if ( a < 0 || b < 0 || c < 0 || a >= nv || b >= nv || c >= nv )
						continue;
					sm.tris.push_back( base + a ); sm.tris.push_back( base + b ); sm.tris.push_back( base + c );
					s.numTris++;
				}
			} else if ( type == MST_PATCH ) {
				int w = LittleLong( in->patchWidth ), ht = LittleLong( in->patchHeight );
				std::vector<mapVert_t> control( nv );

				if ( w < 3 || ht < 3 || w * ht != nv )
					continue;
				for ( int v = 0; v < nv; v++ ) {
					for ( int k = 0; k < 3; k++ )
						control[v].xyz[k] = LittleFloat( verts[fv + v].xyz[k] );
				}
				SM_AddPatch( control.data(), w, ht, &s );
			}

			if ( s.numTris ) {
				SM_SurfBounds( &s );
				sm.surfs.push_back( s );
				bm.numSurfs++;
			}
		}
		sm.bmodels.push_back( bm );
	}

	FS_FreeFile( buf );
	Com_DPrintf( "Shader manager: %s has %i surfaces, %i triangles\n", path, (int)sm.surfs.size(), (int)( sm.tris.size() / 3 ) );
}

static void SM_CheckMap( void ) {
	const char *info, *mapName;

	if ( cl.gameState.stringOffsets[CS_SERVERINFO] <= 0 && !cl.gameState.dataCount )
		return;
	info = cl.gameState.stringData + cl.gameState.stringOffsets[CS_SERVERINFO];
	mapName = Info_ValueForKey( info, "mapname" );
	if ( !mapName[0] )
		return;
	if ( !sm.mapLoaded || Q_stricmp( mapName, sm.mapName ) )
		SM_LoadMap( mapName );
}

/*
===============================================================================

TRACING

===============================================================================
*/

// distance along dir to the triangle, both sides count; -1 if missed
static float SM_RayTriangle( const vec3_t org, const vec3_t dir, const float *a, const float *b, const float *c ) {
	vec3_t e1, e2, p, t, q;
	float det, inv, u, v, d;

	VectorSubtract( b, a, e1 );
	VectorSubtract( c, a, e2 );
	CrossProduct( dir, e2, p );
	det = DotProduct( e1, p );
	if ( fabsf( det ) < 1e-8f )
		return -1.0f;
	inv = 1.0f / det;
	VectorSubtract( org, a, t );
	u = DotProduct( t, p ) * inv;
	if ( u < 0.0f || u > 1.0f )
		return -1.0f;
	CrossProduct( t, e1, q );
	v = DotProduct( dir, q ) * inv;
	if ( v < 0.0f || u + v > 1.0f )
		return -1.0f;
	d = DotProduct( e2, q ) * inv;
	return d > 0.0f ? d : -1.0f;
}

// does the ray reach the box before maxDist
static qboolean SM_RayBox( const vec3_t org, const vec3_t dir, const vec3_t mins, const vec3_t maxs, float maxDist ) {
	float tmin = 0.0f, tmax = maxDist;

	for ( int i = 0; i < 3; i++ ) {
		if ( fabsf( dir[i] ) < 1e-8f ) {
			if ( org[i] < mins[i] - 1.0f || org[i] > maxs[i] + 1.0f )
				return qfalse;
			continue;
		}
		float t1 = ( mins[i] - 1.0f - org[i] ) / dir[i], t2 = ( maxs[i] + 1.0f - org[i] ) / dir[i];

		if ( t1 > t2 ) {
			float tmp = t1;
			t1 = t2;
			t2 = tmp;
		}
		tmin = Q_max( tmin, t1 );
		tmax = Q_min( tmax, t2 );
		if ( tmin > tmax )
			return qfalse;
	}
	return qtrue;
}

// the nearest surface of one map model, in that model's space; -1 if none
static int SM_TraceBModel( int model, const vec3_t org, const vec3_t dir, float *best ) {
	const smBModel_t *bm;
	int found = -1;

	if ( model < 0 || model >= (int)sm.bmodels.size() )
		return -1;
	bm = &sm.bmodels[model];
	for ( int i = bm->firstSurf; i < bm->firstSurf + bm->numSurfs; i++ ) {
		const smSurf_t *s = &sm.surfs[i];

		if ( !SM_RayBox( org, dir, s->mins, s->maxs, *best ) )
			continue;
		for ( int t = s->firstTri; t < s->firstTri + s->numTris; t++ ) {
			const int *tri = &sm.tris[t * 3];
			float d = SM_RayTriangle( org, dir, &sm.verts[tri[0] * 3], &sm.verts[tri[1] * 3], &sm.verts[tri[2] * 3] );

			if ( d > 0.0f && d < *best ) {
				*best = d;
				found = i;
			}
		}
	}
	return found;
}

// the ray in an entity's space; the axes may be scaled
static void SM_ToLocal( const refEntity_t *ent, const vec3_t org, const vec3_t dir, vec3_t lorg, vec3_t ldir ) {
	vec3_t delta;

	VectorSubtract( org, ent->origin, delta );
	for ( int i = 0; i < 3; i++ ) {
		float len2 = DotProduct( ent->axis[i], ent->axis[i] );

		if ( len2 < 1e-8f )
			len2 = 1.0f;
		lorg[i] = DotProduct( delta, ent->axis[i] ) / len2;
		ldir[i] = DotProduct( dir, ent->axis[i] ) / len2;
	}
}

static void SM_SetHit( smHit_t *hit, smHitKind_t kind, float dist, const char *shader, const char *model, const char *surface, const refEntity_t *ent, int surf ) {
	hit->kind = kind;
	hit->dist = dist;
	Q_strncpyz( hit->shader, shader ? shader : "", sizeof( hit->shader ) );
	Q_strncpyz( hit->model, model ? model : "", sizeof( hit->model ) );
	Q_strncpyz( hit->surface, surface ? surface : "", sizeof( hit->surface ) );
	hit->surf = surf;
	if ( ent )
		hit->ent = *ent;
	else
		memset( &hit->ent, 0, sizeof( hit->ent ) );
}

// an md3 or a brush entity
static void SM_TraceModelEntity( const refEntity_t *ent, const vec3_t org, const vec3_t dir, smHit_t *hit ) {
	const char *name;
	vec3_t lorg, ldir;
	float best;

	if ( ent->hModel <= 0 || ent->hModel >= SM_MAX_HANDLES || sm.modelNames[ent->hModel].empty() )
		return;
	name = sm.modelNames[ent->hModel].c_str();
	SM_ToLocal( ent, org, dir, lorg, ldir );

	// the local direction has the axis scale folded in, so distances stay in world units
	if ( name[0] == '*' ) {
		int model = atoi( name + 1 ), surf;

		if ( model <= 0 || model >= (int)sm.bmodels.size() )
			return;
		best = hit->dist;
		surf = SM_TraceBModel( model, lorg, ldir, &best );
		if ( surf >= 0 ) {
			const char *shader = SM_ShaderName( ent->customShader );

			SM_SetHit( hit, SM_HIT_BRUSH, best, shader ? shader : sm.bspShaders[sm.surfs[surf].shader].c_str(), name, NULL, ent, surf );
		}
		return;
	}

	smMd3_t *m = SM_GetMd3( ent->hModel );
	if ( !m )
		return;
	int frame = Com_Clampi( 0, m->numFrames - 1, ent->frame );
	for ( size_t s = 0; s < m->surfs.size(); s++ ) {
		const smMd3Surf_t *surf = &m->surfs[s];
		const float *xyz = &surf->xyz[(size_t)frame * surf->numVerts * 3];
		qboolean hitSurf = qfalse;

		best = hit->dist;
		for ( size_t t = 0; t + 2 < surf->tris.size(); t += 3 ) {
			float d = SM_RayTriangle( lorg, ldir, &xyz[surf->tris[t] * 3], &xyz[surf->tris[t + 1] * 3], &xyz[surf->tris[t + 2] * 3] );

			if ( d > 0.0f && d < best ) {
				best = d;
				hitSurf = qtrue;
			}
		}
		if ( !hitSurf )
			continue;

		// the way R_AddMD3Surfaces picks it
		const char *shader = SM_ShaderName( ent->customShader );
		if ( !shader && ent->customSkin )
			shader = SM_SkinShader( ent->customSkin, surf->name.c_str() );
		if ( !shader && !surf->shaders.empty() )
			shader = surf->shaders[ent->skinNum % surf->shaders.size()].c_str();
		SM_SetHit( hit, SM_HIT_MODEL, best, shader, name, surf->name.c_str(), ent, -1 );
	}
}

static void SM_TraceGhoul2Entity( const refEntity_t *ent, const vec3_t org, const vec3_t dir, smHit_t *hit ) {
	CGhoul2Info_v &ghoul2 = *(CGhoul2Info_v *)ent->ghoul2;
	CollisionRecord_t records[MAX_G2_COLLISIONS];
	vec3_t start, end;
	const CollisionRecord_t *nearest = NULL;

	VectorCopy( org, start );
	VectorMA( org, hit->dist, dir, end );
	memset( records, 0, sizeof( records ) );
	for ( int i = 0; i < MAX_G2_COLLISIONS; i++ )
		records[i].mEntityNum = -1;
	re->G2API_CollisionDetect( records, ghoul2, ent->angles, ent->origin, re->G2API_GetTime( cl.serverTime ), 0,
		start, end, (float *)ent->modelScale, G2VertSpaceClient, G2_FRONTFACE, 0, 0.0f );

	for ( int i = 0; i < MAX_G2_COLLISIONS; i++ ) {
		if ( records[i].mEntityNum == -1 )
			break;
		if ( !nearest || records[i].mDistance < nearest->mDistance )
			nearest = &records[i];
	}
	if ( !nearest || nearest->mDistance >= hit->dist || nearest->mModelIndex < 0 || nearest->mModelIndex >= re->G2API_Ghoul2Size( ghoul2 ) )
		return;

	// the way R_AddGhoulSurfaces picks it
	const char *file = re->G2API_GetModelName( ghoul2, nearest->mModelIndex );
	if ( !file || !file[0] )
		return;
	char fileName[MAX_QPATH];
	Q_strncpyz( fileName, file, sizeof( fileName ) );
	const smGlm_t *glm = SM_GetGlm( fileName );
	const char *surface = nearest->mSurfaceIndex >= 0 && nearest->mSurfaceIndex < (int)glm->surfaces.size() ? glm->surfaces[nearest->mSurfaceIndex].c_str() : NULL;
	const char *shader = SM_ShaderName( ent->customShader );
	qhandle_t customSkin = 0, ownSkin = 0, skin;

	if ( re->ext.G2API_GetSkins )
		re->ext.G2API_GetSkins( ghoul2, nearest->mModelIndex, &customSkin, &ownSkin );
	skin = customSkin ? customSkin : ent->customSkin ? ent->customSkin : ownSkin;
	if ( !shader && skin && surface )
		shader = SM_SkinShader( skin, surface );
	if ( !shader && surface )
		shader = glm->shaders[nearest->mSurfaceIndex].c_str();
	SM_SetHit( hit, SM_HIT_GHOUL2, nearest->mDistance, shader, fileName, surface, ent, -1 );
}

// the player's own model, seen from behind in third person
static qboolean SM_IsSelf( const refEntity_t *ent ) {
	vec3_t delta;

	if ( !cl.snap.valid )
		return qfalse;
	VectorSubtract( ent->origin, cl.snap.ps.origin, delta );
	return (qboolean)( VectorLength( delta ) < SM_SELF_RADIUS );
}

static void SM_Trace( const vec3_t org, const vec3_t dir, smHit_t *hit ) {
	int surf;

	memset( hit, 0, sizeof( *hit ) );
	hit->dist = SM_TRACE_DIST;
	hit->surf = -1;

	SM_CheckMap();
	surf = SM_TraceBModel( 0, org, dir, &hit->dist );
	if ( surf >= 0 )
		SM_SetHit( hit, SM_HIT_WORLD, hit->dist, sm.bspShaders[sm.surfs[surf].shader].c_str(), NULL, NULL, NULL, surf );

	if ( sm.viewFrame == cls.framecount ) {
		for ( int i = 0; i < sm.numEntities; i++ ) {
			const refEntity_t *ent = &sm.entities[i];

			if ( ent->reType != RT_MODEL || ( ent->renderfx & ( RF_FIRST_PERSON | RF_DEPTHHACK | RF_THIRD_PERSON ) ) )
				continue;
			if ( ent->ghoul2 && re->G2API_HaveWeGhoul2Models( *(CGhoul2Info_v *)ent->ghoul2 ) ) {
				// the free camera can look at the player, the normal view only ever sees its back
				if ( sm.state != SM_OFF || !SM_IsSelf( ent ) )
					SM_TraceGhoul2Entity( ent, org, dir, hit );
			} else {
				SM_TraceModelEntity( ent, org, dir, hit );
			}
		}
	}
	VectorMA( org, hit->dist, dir, hit->pos );
}

/*
===============================================================================

REMAPS

===============================================================================
*/

static void SM_SetStatus( const char *text ) {
	Q_strncpyz( sm.status, text, sizeof( sm.status ) );
}

static void SM_Send( const char *cmd ) {
	CL_AddReliableCommand( cmd, qfalse );
	sm.sentTime = cls.realtime;
	sm.reply[0] = '\0';
	Com_Printf( S_COLOR_GREEN "Shader manager: " S_COLOR_WHITE "%s\n", cmd );
	SM_SetStatus( va( "Sent: %s", cmd ) );
}

// "rpshader remap"; record keeps what it was before, for undo
static void SM_SendRemap( const char *from, const char *to, qboolean record ) {
	const char *offset = sm.fields[SM_F_OFFSET], *previous;
	char cmd[MAX_STRING_CHARS];

	if ( !from[0] || !to[0] )
		return;
	if ( record ) {
		previous = SM_RemappedTo( from );
		sm.undoShader.push_back( from );
		sm.undoPrevious.push_back( previous ? previous : from );
		if ( (int)sm.undoShader.size() > SM_MAX_HISTORY ) {
			sm.undoShader.erase( sm.undoShader.begin() );
			sm.undoPrevious.erase( sm.undoPrevious.begin() );
		}
	}
	if ( offset[0] && Q_stricmp( from, to ) )
		Com_sprintf( cmd, sizeof( cmd ), "rpshader remap %s %s %s", from, to, offset );
	else
		Com_sprintf( cmd, sizeof( cmd ), "rpshader remap %s %s", from, to );
	SM_Send( cmd );
}

static void SM_Undo( void ) {
	std::string shader, previous;

	if ( sm.undoShader.empty() )
		return;
	shader = sm.undoShader.back();
	previous = sm.undoPrevious.back();
	sm.undoShader.pop_back();
	sm.undoPrevious.pop_back();
	SM_SendRemap( shader.c_str(), previous.c_str(), qfalse );
}

// the remap shown on this screen only, undone by putting back what the server has
static void SM_ClearLive( void ) {
	const char *target;

	if ( !sm.liveFrom[0] )
		return;
	target = SM_RemappedTo( sm.liveFrom );
	re->RemapShader( sm.liveFrom, target ? target : sm.liveFrom, NULL );
	sm.liveFrom[0] = sm.liveTo[0] = '\0';
}

static void SM_SetLive( const char *from, const char *to ) {
	if ( from && to && !Q_stricmp( from, sm.liveFrom ) && !Q_stricmp( to, sm.liveTo ) )
		return;
	SM_ClearLive();
	if ( !from || !to || !from[0] || !to[0] )
		return;
	re->RemapShader( from, to, sm.fields[SM_F_OFFSET][0] ? sm.fields[SM_F_OFFSET] : NULL );
	Q_strncpyz( sm.liveFrom, from, sizeof( sm.liveFrom ) );
	Q_strncpyz( sm.liveTo, to, sizeof( sm.liveTo ) );
}

// a print from the server soon after a command is taken as its answer
void CL_ShaderManager_ServerPrint( const char *text ) {
	char line[MAX_STRING_CHARS];
	int len;

	if ( sm.state == SM_OFF || !sm.sentTime || cls.realtime - sm.sentTime > SM_REPLY_MS )
		return;
	Q_strncpyz( line, text, sizeof( line ) );
	Q_StripColor( line );
	len = strlen( line );
	while ( len && ( line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' ' ) )
		line[--len] = '\0';
	if ( !line[0] )
		return;
	Q_strncpyz( sm.reply, line, sizeof( sm.reply ) );
	sm.replyTime = cls.realtime;
}

/*
===============================================================================

SHADER INDEX

===============================================================================
*/

static void SM_AddShaderName( const char *name ) {
	char key[MAX_QPATH];

	COM_StripExtension( name, key, sizeof( key ) );
	for ( char *p = key; *p; p++ ) {
		if ( *p == '\\' )
			*p = '/';
	}
	if ( key[0] )
		sm.shaderNames.push_back( key );
}

static void SM_AddImage( const char *name, void *ctx ) {
	SM_AddShaderName( name );
}

// the names in one .shader file
static void SM_ParseShaderFile( const char *path ) {
	char *buf;
	const char *p, *token;
	char name[MAX_QPATH];

	if ( FS_ReadFile( path, (void **)&buf ) <= 0 || !buf )
		return;
	p = buf;
	COM_BeginParseSession( path );
	while ( 1 ) {
		token = COM_ParseExt( &p, qtrue );
		if ( !token[0] )
			break;
		Q_strncpyz( name, token, sizeof( name ) );
		token = COM_ParseExt( &p, qtrue );
		if ( Q_stricmp( token, "{" ) )
			break;
		SkipBracedSection( &p, 1 );
		SM_AddShaderName( name );
	}
	FS_FreeFile( buf );
}

static bool SM_NameLess( const std::string &a, const std::string &b ) {
	return Q_stricmp( a.c_str(), b.c_str() ) < 0;
}

// every shader in scripts/*.shader, and every image a shader can be made from
static void SM_BuildIndex( void ) {
	static const char *dirs[] = { "textures", "models", "gfx" };
	static const char *exts[] = { ".jpg", ".tga", ".png" };
	int start = Sys_Milliseconds(), numFiles, out = 0;
	char **files;

	sm.shaderNames.clear();
	files = FS_ListFiles( "scripts", ".shader", &numFiles );
	for ( int i = 0; i < numFiles; i++ )
		SM_ParseShaderFile( va( "scripts/%s", files[i] ) );
	FS_FreeFileList( files );
	for ( size_t d = 0; d < ARRAY_LEN( dirs ); d++ ) {
		for ( size_t e = 0; e < ARRAY_LEN( exts ); e++ )
			FS_ListFilesRecursive( dirs[d], exts[e], SM_AddImage, NULL );
	}

	std::sort( sm.shaderNames.begin(), sm.shaderNames.end(), SM_NameLess );
	for ( size_t i = 0; i < sm.shaderNames.size(); i++ ) {
		if ( !out || Q_stricmp( sm.shaderNames[out - 1].c_str(), sm.shaderNames[i].c_str() ) )
			sm.shaderNames[out++] = sm.shaderNames[i];
	}
	sm.shaderNames.resize( out );
	sm.indexed = qtrue;
	sm.shaderList.sel = sm.shaderList.scroll = 0;
	Com_Printf( "Shader manager: indexed %i shaders (%i ms)\n", out, Sys_Milliseconds() - start );
}

static qboolean SM_ContainsNoCase( const char *haystack, const char *needle ) {
	int len = strlen( needle );

	if ( !len )
		return qtrue;
	for ( ; *haystack; haystack++ ) {
		if ( !Q_stricmpn( haystack, needle, len ) )
			return qtrue;
	}
	return qfalse;
}

static void SM_RebuildShaderView( void ) {
	static const char *prefixes[] = { "", "textures/", "models/", "gfx/" };
	const char *search = sm.fields[SM_F_SEARCH], *prefix = prefixes[sm.filter];
	int prefixLen = strlen( prefix ), oldSel = -1;

	if ( sm.shaderList.sel >= 0 && sm.shaderList.sel < (int)sm.shaderView.size() )
		oldSel = sm.shaderView[sm.shaderList.sel];
	sm.shaderView.clear();
	for ( size_t i = 0; i < sm.shaderNames.size(); i++ ) {
		const char *name = sm.shaderNames[i].c_str();

		if ( prefixLen && Q_stricmpn( name, prefix, prefixLen ) )
			continue;
		if ( SM_ContainsNoCase( name, search ) )
			sm.shaderView.push_back( (int)i );
	}
	// keep the highlighted shader if it is still there
	sm.shaderList.sel = 0;
	for ( size_t i = 0; i < sm.shaderView.size(); i++ ) {
		if ( sm.shaderView[i] == oldSel ) {
			sm.shaderList.sel = (int)i;
			break;
		}
	}
}

static const char *SM_HighlightedShader( void ) {
	if ( sm.shaderList.sel < 0 || sm.shaderList.sel >= (int)sm.shaderView.size() )
		return NULL;
	return sm.shaderNames[sm.shaderView[sm.shaderList.sel]].c_str();
}

static const char *SM_SelectedOriginal( void ) {
	if ( !sm.haveSel || sm.selList.sel < 0 || sm.selList.sel >= (int)sm.selShaders.size() )
		return NULL;
	return sm.selShaders[sm.selList.sel].c_str();
}

// a 2D handle to preview a shader by, registered the first time it is shown
static qhandle_t SM_PreviewHandle( const char *name ) {
	char key[MAX_QPATH];
	std::map<std::string, qhandle_t>::iterator it;
	qhandle_t h;

	SM_ShaderKey( name, key, sizeof( key ) );
	it = sm.previews.find( key );
	if ( it != sm.previews.end() )
		return it->second;
	h = re->RegisterShaderNoMip( name );
	sm.previews[key] = h;
	return h;
}

/*
===============================================================================

SELECTION

===============================================================================
*/

static void SM_AddSelShader( const char *shader ) {
	if ( !shader || !shader[0] || shader[0] == '*' )
		return;	// "*off" in a skin
	for ( size_t i = 0; i < sm.selShaders.size(); i++ ) {
		if ( !Q_stricmp( sm.selShaders[i].c_str(), shader ) )
			return;
	}
	sm.selShaders.push_back( shader );
}

// every shader on what was hit, the hit one selected
static void SM_Select( const smHit_t *hit ) {
	const refEntity_t *ent = &hit->ent;
	const char *custom = SM_ShaderName( ent->customShader );

	sm.haveSel = qtrue;
	sm.selKind = hit->kind;
	sm.selShaders.clear();
	sm.selList.sel = sm.selList.scroll = 0;
	sm.selSurf = hit->surf;
	sm.selBModel[0] = '\0';

	switch ( hit->kind ) {
	case SM_HIT_WORLD:
		Q_strncpyz( sm.selWhat, "Map surface", sizeof( sm.selWhat ) );
		SM_AddSelShader( hit->shader );
		break;

	case SM_HIT_BRUSH: {
		int model = atoi( hit->model + 1 );

		Com_sprintf( sm.selWhat, sizeof( sm.selWhat ), "Brush entity %s", hit->model );
		Q_strncpyz( sm.selBModel, hit->model, sizeof( sm.selBModel ) );
		SM_AddSelShader( hit->shader );
		if ( !custom && model > 0 && model < (int)sm.bmodels.size() ) {
			const smBModel_t *bm = &sm.bmodels[model];

			for ( int i = bm->firstSurf; i < bm->firstSurf + bm->numSurfs; i++ )
				SM_AddSelShader( sm.bspShaders[sm.surfs[i].shader].c_str() );
		}
		break;
	}

	case SM_HIT_MODEL: {
		smMd3_t *m = SM_GetMd3( ent->hModel );

		Q_strncpyz( sm.selWhat, hit->model, sizeof( sm.selWhat ) );
		SM_AddSelShader( hit->shader );
		for ( size_t s = 0; m && !custom && s < m->surfs.size(); s++ ) {
			const smMd3Surf_t *surf = &m->surfs[s];
			const char *shader = ent->customSkin ? SM_SkinShader( ent->customSkin, surf->name.c_str() ) : NULL;

			if ( !shader && !surf->shaders.empty() )
				shader = surf->shaders[ent->skinNum % surf->shaders.size()].c_str();
			SM_AddSelShader( shader );
		}
		break;
	}

	case SM_HIT_GHOUL2: {
		CGhoul2Info_v &ghoul2 = *(CGhoul2Info_v *)ent->ghoul2;
		int numModels = re->G2API_Ghoul2Size( ghoul2 );

		Q_strncpyz( sm.selWhat, hit->model, sizeof( sm.selWhat ) );
		SM_AddSelShader( hit->shader );
		for ( int m = 0; !custom && m < numModels; m++ ) {
			const char *file = re->G2API_GetModelName( ghoul2, m );
			qhandle_t customSkin = 0, ownSkin = 0, skin;
			char fileName[MAX_QPATH];

			if ( !file || !file[0] )
				continue;
			Q_strncpyz( fileName, file, sizeof( fileName ) );
			const smGlm_t *glm = SM_GetGlm( fileName );
			if ( re->ext.G2API_GetSkins )
				re->ext.G2API_GetSkins( ghoul2, m, &customSkin, &ownSkin );
			skin = customSkin ? customSkin : ent->customSkin ? ent->customSkin : ownSkin;
			for ( size_t s = 0; s < glm->surfaces.size(); s++ ) {
				const char *surface = glm->surfaces[s].c_str(), *shader = NULL;
				int status = re->G2API_GetSurfaceRenderStatus( ghoul2, m, surface );

				if ( status < 0 || ( status & ( G2SURFACEFLAG_OFF | G2SURFACEFLAG_NODESCENDANTS ) ) )
					continue;
				if ( skin )
					shader = SM_SkinShader( skin, surface );
				if ( !shader )
					shader = glm->shaders[s].c_str();
				SM_AddSelShader( shader );
			}
		}
		break;
	}

	default:
		sm.haveSel = qfalse;
		return;
	}
	if ( custom )
		Q_strcat( sm.selWhat, sizeof( sm.selWhat ), "  (forced shader)" );
}

// a shader from the remaps tab, edited without an object
static void SM_SelectShader( const char *shader ) {
	sm.haveSel = qtrue;
	sm.selKind = SM_HIT_NONE;
	sm.selShaders.clear();
	sm.selShaders.push_back( shader );
	sm.selList.sel = sm.selList.scroll = 0;
	sm.selSurf = -1;
	sm.selBModel[0] = '\0';
	Q_strncpyz( sm.selWhat, "From the remap list", sizeof( sm.selWhat ) );
}

/*
===============================================================================

STATE CHANGES

===============================================================================
*/

static void SM_ReleaseKeys( void ) {
	memset( sm.held, 0, sizeof( sm.held ) );
	sm.looking = qfalse;
}

static void SM_EnterCamera( void ) {
	SM_ClearLive();
	sm.state = SM_CAMERA;
	sm.focus = SM_F_NONE;
	SM_ReleaseKeys();
}

static void SM_EnterEdit( smTab_t tab ) {
	if ( !sm.indexed )
		SM_BuildIndex();
	sm.state = SM_EDIT;
	sm.tab = tab;
	sm.focus = SM_F_NONE;
	sm.cursorX = SM_PANEL_X * 0.5f;
	sm.cursorY = SCREEN_HEIGHT * 0.5f;
	sm.click = qfalse;
	sm.wheel = 0;
	SM_ReleaseKeys();
	SM_RebuildShaderView();
}

static void SM_Open( void ) {
	if ( cls.state != CA_ACTIVE || !cls.cgameStarted ) {
		Com_Printf( "Shader manager: join a server first\n" );
		return;
	}
	CL_ModelPlacer_Close();
	CL_NpcManager_Close();
	CL_EffectManager_Close();

	sm.font = re->RegisterFont( "arialnb" );
	if ( !sm.font )
		sm.font = cls.menuFont;
	if ( sm.haveView ) {
		VectorCopy( sm.viewOrg, sm.camOrg );
		vectoangles( sm.viewAxis[0], sm.camAng );
	} else {
		VectorCopy( cl.snap.ps.origin, sm.camOrg );
		sm.camOrg[2] += cl.snap.ps.viewheight;
		VectorCopy( cl.viewangles, sm.camAng );
	}
	sm.camAng[ROLL] = 0;
	if ( sm.camAng[PITCH] > 180.0f )
		sm.camAng[PITCH] -= 360.0f;
	sm.lastFrameTime = cls.realtime;
	SM_EnterCamera();
	Key_SetCatcher( Key_GetCatcher() | KEYCATCH_SHADERMANAGER );
}

void CL_ShaderManager_Close( void ) {
	if ( sm.state == SM_OFF )
		return;
	SM_ClearLive();
	sm.state = SM_OFF;
	SM_ReleaseKeys();
	Key_SetCatcher( Key_GetCatcher() & ~KEYCATCH_SHADERMANAGER );
}

qboolean CL_ShaderManager_Active( void ) {
	return (qboolean)( sm.state != SM_OFF );
}

/*
===============================================================================

INPUT

===============================================================================
*/

// binds don't run while the manager holds the keys, so honour the user's toggle bind here
static qboolean SM_IsToggleKey( int key ) {
	const char *binding;

	// a printable key belongs to the search box while editing
	if ( sm.state == SM_EDIT && !sm.looking && key >= A_SPACE && key <= A_TILDE )
		return qfalse;
	binding = Key_GetBinding( key );
	return (qboolean)( VALIDSTRING( binding ) && !Q_stricmp( binding, SM_TOGGLE_CMD ) );
}

static void SM_ListMove( smList_t *list, int count, int visible, int delta ) {
	list->sel = Com_Clampi( 0, Q_max( count - 1, 0 ), list->sel + delta );
	if ( list->sel < list->scroll )
		list->scroll = list->sel;
	else if ( list->sel >= list->scroll + visible )
		list->scroll = list->sel - visible + 1;
}

static int SM_ShaderRows( void ) {
	return sm.showPreview ? 11 : 17;
}

static void SM_EditKey( int key ) {
	smList_t *list;
	int count, visible, delta, len;
	smField_t f = sm.focus != SM_F_NONE ? sm.focus : SM_F_SEARCH;

	switch ( key ) {
	case A_MOUSE1:
		sm.click = qtrue;
		sm.clickX = sm.cursorX;
		sm.clickY = sm.cursorY;
		return;
	case A_MOUSE2:
		sm.looking = qtrue;
		return;
	case A_MWHEELUP:
		sm.wheel -= 3;
		return;
	case A_MWHEELDOWN:
		sm.wheel += 3;
		return;
	case A_TAB:
		sm.tab = sm.tab == SM_TAB_OBJECT ? SM_TAB_REMAPS : SM_TAB_OBJECT;
		sm.focus = SM_F_NONE;
		return;
	case A_ENTER:
	case A_KP_ENTER:
		if ( sm.tab == SM_TAB_OBJECT && SM_SelectedOriginal() && SM_HighlightedShader() )
			SM_SendRemap( SM_SelectedOriginal(), SM_HighlightedShader(), qtrue );
		return;
	case A_BACKSPACE:
		len = strlen( sm.fields[f] );
		if ( len ) {
			sm.fields[f][len - 1] = '\0';
			if ( f == SM_F_SEARCH )
				SM_RebuildShaderView();
		}
		return;
	case A_DELETE:
		sm.fields[f][0] = '\0';
		if ( f == SM_F_SEARCH )
			SM_RebuildShaderView();
		return;
	default:
		break;
	}

	if ( sm.tab == SM_TAB_OBJECT ) {
		list = &sm.shaderList;
		count = (int)sm.shaderView.size();
		visible = SM_ShaderRows();
	} else {
		list = &sm.remapList;
		count = (int)sm.remapFrom.size();
		visible = 26;
	}
	switch ( key ) {
	case A_CURSOR_UP:	delta = -1; break;
	case A_CURSOR_DOWN:	delta = 1; break;
	case A_PAGE_UP:		delta = -visible; break;
	case A_PAGE_DOWN:	delta = visible; break;
	case A_HOME:		delta = -count; break;
	case A_END:			delta = count; break;
	default:			return;
	}
	SM_ListMove( list, count, visible, delta );
}

static void SM_SelectUnderRay( void );

void CL_ShaderManager_KeyEvent( int key, qboolean down ) {
	if ( sm.state == SM_OFF )
		return;

	if ( key >= 0 && key < MAX_KEYS )
		sm.held[key] = down;
	if ( !down ) {
		if ( key == A_MOUSE2 )
			sm.looking = qfalse;
		return;
	}

	if ( SM_IsToggleKey( key ) ) {
		CL_ShaderManager_Close();
		return;
	}

	if ( sm.state == SM_CAMERA ) {
		if ( key == A_MOUSE1 )
			sm.click = qtrue;	// picked in Draw, where this frame's trace is
		else if ( key == A_TAB )
			SM_EnterEdit( SM_TAB_REMAPS );
		return;
	}
	if ( sm.looking && key != A_MOUSE1 )
		return;	// flying keys
	SM_EditKey( key );
}

void CL_ShaderManager_CharEvent( int ch ) {
	smField_t f;
	int len;

	if ( sm.state != SM_EDIT || sm.looking || ch < ' ' || ch > '~' )
		return;
	f = sm.focus != SM_F_NONE ? sm.focus : SM_F_SEARCH;
	if ( f == SM_F_OFFSET && !( isdigit( ch ) || ch == '.' || ch == '-' ) )
		return;
	len = strlen( sm.fields[f] );
	if ( len < SM_FIELD_LEN - 1 ) {
		sm.fields[f][len] = (char)ch;
		sm.fields[f][len + 1] = '\0';
		if ( f == SM_F_SEARCH ) {
			sm.shaderList.scroll = 0;
			SM_RebuildShaderView();
		}
	}
}

void CL_ShaderManager_MouseEvent( int dx, int dy ) {
	if ( sm.state == SM_CAMERA || ( sm.state == SM_EDIT && sm.looking ) ) {
		sm.camAng[YAW] -= dx * cl_sensitivity->value * m_yaw->value;
		sm.camAng[PITCH] = Com_Clamp( -89.0f, 89.0f, sm.camAng[PITCH] + dy * cl_sensitivity->value * m_pitch->value );
	} else if ( sm.state == SM_EDIT ) {
		sm.cursorX = Com_Clamp( 0, SCREEN_WIDTH, sm.cursorX + dx );
		sm.cursorY = Com_Clamp( 0, SCREEN_HEIGHT, sm.cursorY + dy );
	}
}

// escape backs out one level: a text box, the panel, then the manager
void CL_ShaderManager_Escape( void ) {
	if ( sm.state == SM_EDIT && sm.focus != SM_F_NONE )
		sm.focus = SM_F_NONE;
	else if ( sm.state == SM_EDIT )
		SM_EnterCamera();
	else
		CL_ShaderManager_Close();
}

/*
===============================================================================

CAMERA AND SCENE

===============================================================================
*/

static qboolean SM_ShiftDown( void ) {
	return (qboolean)( sm.held[A_SHIFT] || sm.held[A_SHIFT2] );
}

static qboolean SM_CtrlDown( void ) {
	return (qboolean)( sm.held[A_CTRL] || sm.held[A_CTRL2] );
}

// called every frame before the cgame draws
void CL_ShaderManager_Frame( void ) {
	vec3_t forward, right, move;
	float dt, speed;

	if ( sm.state == SM_OFF )
		return;
	if ( !( Key_GetCatcher() & KEYCATCH_SHADERMANAGER ) ) {
		// something else cleared the catchers
		SM_ClearLive();
		sm.state = SM_OFF;
		SM_ReleaseKeys();
		return;
	}

	dt = Com_Clamp( 0.0f, 0.1f, ( cls.realtime - sm.lastFrameTime ) / 1000.0f );
	sm.lastFrameTime = cls.realtime;
	if ( sm.state == SM_EDIT && !sm.looking )
		return;

	AngleVectors( sm.camAng, forward, right, NULL );
	VectorClear( move );
	if ( sm.held[A_CAP_W] )		VectorAdd( move, forward, move );
	if ( sm.held[A_CAP_S] )		VectorSubtract( move, forward, move );
	if ( sm.held[A_CAP_D] )		VectorAdd( move, right, move );
	if ( sm.held[A_CAP_A] )		VectorSubtract( move, right, move );
	if ( sm.held[A_SPACE] )		move[2] += 1.0f;
	if ( sm.held[A_CAP_C] )		move[2] -= 1.0f;
	if ( VectorNormalize( move ) > 0.0f ) {
		speed = SM_FLY_SPEED;
		if ( SM_ShiftDown() )
			speed *= 3.0f;
		if ( SM_CtrlDown() )
			speed *= 0.25f;
		VectorMA( sm.camOrg, speed * dt, move, sm.camOrg );
	}
}

// every entity cgame adds; kept for the scene that follows
void CL_ShaderManager_AddEntity( const refEntity_t *ent ) {
	if ( sm.state == SM_OFF && !cl_shaderCrosshair->integer )
		return;
	if ( sm.numPending < SM_MAX_ENTITIES )
		sm.pending[sm.numPending++] = *ent;
}

// the free camera, for the effects system to cull against
qboolean CL_ShaderManager_Camera( vec3_t origin, vec3_t angles ) {
	if ( sm.state == SM_OFF )
		return qfalse;
	VectorCopy( sm.camOrg, origin );
	VectorCopy( sm.camAng, angles );
	return qtrue;
}

// the view weapon would float where the player stands
qboolean CL_ShaderManager_FilterEntity( const refEntity_t *ent ) {
	return (qboolean)( sm.state != SM_OFF && ( ent->renderfx & RF_FIRST_PERSON ) );
}

static void SM_KeepView( const refdef_t *view, float x, float w ) {
	sm.haveView = qtrue;
	sm.viewFrame = cls.framecount;
	VectorCopy( view->vieworg, sm.viewOrg );
	VectorCopy( view->viewaxis[0], sm.viewAxis[0] );
	VectorCopy( view->viewaxis[1], sm.viewAxis[1] );
	VectorCopy( view->viewaxis[2], sm.viewAxis[2] );
	sm.viewFovX = view->fov_x;
	sm.viewFovY = view->fov_y;
	sm.viewX = x;
	sm.viewW = w;
	memcpy( sm.entities, sm.pending, sm.numPending * sizeof( sm.pending[0] ) );
	sm.numEntities = sm.numPending;
}

// the camera, and with the panel open only the part of the screen left of it
static void SM_AimView( refdef_t *view ) {
	AnglesToAxis( sm.camAng, view->viewaxis );
	VectorCopy( sm.camAng, view->viewangles );
	if ( sm.state == SM_EDIT ) {
		view->width = (int)( view->width * SM_PANEL_X / SCREEN_WIDTH );
		view->fov_x = RAD2DEG( 2.0f * atanf( tanf( DEG2RAD( view->fov_y * 0.5f ) ) * view->width / (float)view->height ) );
	}
}

// a thin bar both ways round, as the model placer draws its box
static void SM_AddEdge( const vec3_t a, const vec3_t b, const byte *color ) {
	polyVert_t verts[4], back[4];
	vec3_t dir, toCam, side, mid;
	float width;

	VectorSubtract( b, a, dir );
	VectorAdd( a, b, mid );
	VectorScale( mid, 0.5f, mid );
	VectorSubtract( sm.camOrg, mid, toCam );
	width = 0.3f + VectorLength( toCam ) * 0.001f;
	CrossProduct( dir, toCam, side );
	if ( VectorNormalize( side ) <= 0.0f )
		return;
	VectorScale( side, width, side );

	VectorAdd( a, side, verts[0].xyz );
	VectorAdd( b, side, verts[1].xyz );
	VectorSubtract( b, side, verts[2].xyz );
	VectorSubtract( a, side, verts[3].xyz );
	for ( int i = 0; i < 4; i++ ) {
		verts[i].st[0] = ( i == 1 || i == 2 ) ? 1.0f : 0.0f;
		verts[i].st[1] = ( i >= 2 ) ? 1.0f : 0.0f;
		memcpy( verts[i].modulate, color, sizeof( verts[i].modulate ) );
	}
	for ( int i = 0; i < 4; i++ )
		back[i] = verts[3 - i];
	re->AddPolyToScene( cls.whiteShader, 4, verts, 1 );
	re->AddPolyToScene( cls.whiteShader, 4, back, 1 );
}

static bool SM_EdgeLess( const std::pair<int, int> &a, const std::pair<int, int> &b ) {
	return a.first != b.first ? a.first < b.first : a.second < b.second;
}

// the border of one map surface; a brush entity's moves with it
static void SM_AddOutline( int surfIndex, const char *bmodel, const byte *color ) {
	const refEntity_t *ent = NULL;
	std::vector<std::pair<int, int> > edges;
	const smSurf_t *s;

	if ( surfIndex < 0 || surfIndex >= (int)sm.surfs.size() )
		return;
	s = &sm.surfs[surfIndex];
	if ( s->numTris > SM_MAX_OUTLINE )
		return;
	if ( bmodel && bmodel[0] ) {
		for ( int i = 0; i < sm.numPending && !ent; i++ ) {
			const refEntity_t *e = &sm.pending[i];

			if ( e->hModel > 0 && e->hModel < SM_MAX_HANDLES && !Q_stricmp( sm.modelNames[e->hModel].c_str(), bmodel ) )
				ent = e;
		}
		if ( !ent )
			return;
	}

	// edges used by one triangle only are the border
	for ( int t = s->firstTri; t < s->firstTri + s->numTris; t++ ) {
		for ( int k = 0; k < 3; k++ ) {
			int a = sm.tris[t * 3 + k], b = sm.tris[t * 3 + ( k + 1 ) % 3];

			edges.push_back( std::make_pair( Q_min( a, b ), Q_max( a, b ) ) );
		}
	}
	std::sort( edges.begin(), edges.end(), SM_EdgeLess );
	for ( size_t i = 0; i < edges.size(); ) {
		size_t j = i;
		vec3_t p[2];

		while ( j < edges.size() && edges[j] == edges[i] )
			j++;
		if ( j - i == 1 ) {
			for ( int k = 0; k < 2; k++ ) {
				const float *v = &sm.verts[( k ? edges[i].second : edges[i].first ) * 3];

				if ( ent ) {
					VectorCopy( ent->origin, p[k] );
					for ( int a = 0; a < 3; a++ )
						VectorMA( p[k], v[a], ent->axis[a], p[k] );
				} else {
					VectorCopy( v, p[k] );
				}
			}
			SM_AddEdge( p[0], p[1], color );
		}
		i = j;
	}
}

// every cgame scene passes through here; qfalse leaves it to the model placer
qboolean CL_ShaderManager_RenderScene( const refdef_t *fd ) {
	static const byte hoverColor[4] = { 255, 220, 64, 255 };
	static const byte selColor[4] = { 64, 220, 255, 255 };
	refdef_t view;

	if ( fd->rdflags & ( RDF_NOWORLDMODEL | RDF_AUTOMAP ) ) {
		sm.numPending = 0;
		return qfalse;
	}

	if ( sm.state == SM_OFF ) {
		if ( !( fd->rdflags & RDF_SKYBOXPORTAL ) )
			SM_KeepView( fd, 0.0f, SCREEN_WIDTH );
		sm.numPending = 0;
		return qfalse;
	}

	view = *fd;
	SM_AimView( &view );
	if ( fd->rdflags & RDF_SKYBOXPORTAL ) {
		// the sky portal keeps its own origin but looks the way the camera does
		re->RenderScene( &view );
		sm.numPending = 0;
		return qtrue;
	}

	// the main view, once a frame
	if ( sm.renderedFrame == cls.framecount ) {
		re->RenderScene( &view );
		sm.numPending = 0;
		return qtrue;
	}
	sm.renderedFrame = cls.framecount;

	VectorCopy( sm.camOrg, view.vieworg );
	view.viewContents = CM_PointContents( sm.camOrg, 0 );
	// the snapshot's area mask is from the player's position, the camera can be anywhere
	memset( view.areamask, 0, sizeof( view.areamask ) );

	if ( sm.hitValid && sm.hitFrame >= cls.framecount - 1 && sm.hit.surf >= 0 )
		SM_AddOutline( sm.hit.surf, sm.hit.kind == SM_HIT_BRUSH ? sm.hit.model : NULL, hoverColor );
	if ( sm.state == SM_EDIT && sm.haveSel && sm.selSurf >= 0 && ( cls.realtime / 400 ) % 2 )
		SM_AddOutline( sm.selSurf, sm.selBModel, selColor );

	SM_KeepView( &view, 0.0f, sm.state == SM_EDIT ? SM_PANEL_X : SCREEN_WIDTH );
	re->RenderScene( &view );
	sm.numPending = 0;
	return qtrue;
}

// the ray under the crosshair, or under the mouse when the panel is open
static qboolean SM_PickRay( vec3_t org, vec3_t dir ) {
	float nx, ny, centerY = SCREEN_HEIGHT * 0.5f;

	if ( !sm.haveView )
		return qfalse;
	VectorCopy( sm.viewOrg, org );
	if ( sm.state != SM_EDIT ) {
		VectorCopy( sm.viewAxis[0], dir );
		return qtrue;
	}
	if ( sm.cursorX >= sm.viewX + sm.viewW || sm.cursorX < sm.viewX || sm.looking )
		return qfalse;
	nx = ( sm.cursorX - ( sm.viewX + sm.viewW * 0.5f ) ) / ( sm.viewW * 0.5f );
	ny = ( sm.cursorY - centerY ) / centerY;
	VectorCopy( sm.viewAxis[0], dir );
	VectorMA( dir, -nx * tanf( DEG2RAD( sm.viewFovX * 0.5f ) ), sm.viewAxis[1], dir );
	VectorMA( dir, -ny * tanf( DEG2RAD( sm.viewFovY * 0.5f ) ), sm.viewAxis[2], dir );
	VectorNormalize( dir );
	return qtrue;
}

static void SM_UpdateHit( void ) {
	vec3_t org, dir;

	if ( sm.hitFrame == cls.framecount )
		return;
	sm.hitFrame = cls.framecount;
	sm.hitValid = SM_PickRay( org, dir );
	if ( sm.hitValid )
		SM_Trace( org, dir, &sm.hit );
}

static void SM_SelectUnderRay( void ) {
	SM_UpdateHit();
	if ( !sm.hitValid || sm.hit.kind == SM_HIT_NONE || !sm.hit.shader[0] )
		return;
	SM_Select( &sm.hit );
	if ( !sm.haveSel )
		return;
	if ( sm.state != SM_EDIT )
		SM_EnterEdit( SM_TAB_OBJECT );
	sm.tab = SM_TAB_OBJECT;
	SM_SetStatus( "" );
}

/*
===============================================================================

2D

===============================================================================
*/

static vec4_t smRed			= { 1.0f, 0.3f, 0.3f, 1.0f };
static vec4_t smPanelLight	= { 0.15f, 0.15f, 0.15f, 0.85f };
static vec4_t smPanelDark	= { 0.0f, 0.0f, 0.0f, 0.85f };
static vec4_t smPanelFocus	= { 0.1f, 0.2f, 0.3f, 0.9f };
static vec4_t smHighlight	= { 0.2f, 0.45f, 0.8f, 0.8f };
static vec4_t smHover		= { 1.0f, 1.0f, 1.0f, 0.12f };
static vec4_t smHoverBox	= { 0.25f, 0.25f, 0.25f, 0.9f };
static vec4_t smBorder		= { 0.5f, 0.5f, 0.5f, 0.8f };
static vec4_t smDim			= { 0.7f, 0.7f, 0.7f, 1.0f };
static vec4_t smChecker		= { 0.3f, 0.3f, 0.3f, 1.0f };

static void SM_Fill( float x, float y, float w, float h, const float *color ) {
	re->SetColor( color );
	re->DrawStretchPic( x, y, w, h, 0, 0, 0, 0, cls.whiteShader );
	re->SetColor( NULL );
}

static void SM_Box( float x, float y, float w, float h, const float *fill ) {
	SM_Fill( x, y, w, h, fill );
	SM_Fill( x, y, w, 1, smBorder );
	SM_Fill( x, y + h - 1, w, 1, smBorder );
	SM_Fill( x, y, 1, h, smBorder );
	SM_Fill( x + w - 1, y, 1, h, smBorder );
}

static void SM_Text( float x, float y, const char *text, const float *color ) {
	re->Font_DrawString( (int)x, (int)y, text, color, sm.font | STYLE_DROPSHADOW, -1, SM_TEXT_SCALE );
}

static float SM_TextWidth( const char *text ) {
	return (float)re->Font_StrLenPixels( text, sm.font, SM_TEXT_SCALE );
}

static void SM_TextClipped( float x, float y, float w, const char *text, const float *color ) {
	char buf[MAX_STRING_CHARS];
	int len;

	Q_strncpyz( buf, text, sizeof( buf ) );
	len = strlen( buf );
	while ( len > 3 && SM_TextWidth( buf ) > w ) {
		len--;
		buf[len - 3] = '.';
		buf[len - 2] = '.';
		buf[len - 1] = '.';
		buf[len] = '\0';
	}
	SM_Text( x, y, buf, color );
}

static qboolean SM_InRect( float x, float y, float w, float h ) {
	return (qboolean)( sm.cursorX >= x && sm.cursorX < x + w && sm.cursorY >= y && sm.cursorY < y + h );
}

// eats this frame's click if it landed in the rect
static qboolean SM_Clicked( float x, float y, float w, float h ) {
	if ( !sm.click || sm.clickX < x || sm.clickX >= x + w || sm.clickY < y || sm.clickY >= y + h )
		return qfalse;
	sm.click = qfalse;
	return qtrue;
}

static qboolean SM_Button( float x, float y, float w, const char *label, qboolean selected, qboolean enabled ) {
	const float *fill = selected ? smHighlight : ( enabled && SM_InRect( x, y, w, SM_CTRL_H ) ) ? smHoverBox : smPanelLight;

	SM_Box( x, y, w, SM_CTRL_H, fill );
	SM_Text( x + ( w - SM_TextWidth( label ) ) * 0.5f, y + 2, label, enabled ? smWhite : smDim );
	return (qboolean)( SM_Clicked( x, y, w, SM_CTRL_H ) && enabled );
}

static void SM_Field( smField_t f, float x, float y, float w, const char *placeholder ) {
	qboolean active = (qboolean)( sm.focus == f || ( f == SM_F_SEARCH && sm.focus == SM_F_NONE ) );

	SM_Box( x, y, w, SM_CTRL_H, active ? smPanelFocus : smPanelLight );
	if ( sm.fields[f][0] )
		SM_TextClipped( x + 4, y + 2, w - 12, sm.fields[f], smWhite );
	else
		SM_TextClipped( x + 4, y + 2, w - 8, placeholder, smDim );
	if ( active && ( cls.realtime / 500 ) % 2 )
		SM_Text( x + 4 + Q_min( SM_TextWidth( sm.fields[f] ), w - 12 ), y + 2, "_", smWhite );
	if ( SM_Clicked( x, y, w, SM_CTRL_H ) )
		sm.focus = f;
}

typedef void ( *smRowFunc_t )( int row, float x, float y, float w );

// returns the row clicked, -1 if none
static int SM_List( smList_t *list, int count, float x, float y, float w, int rows, smRowFunc_t drawRow ) {
	const float h = rows * SM_ROW_H;
	int row, clicked = -1;

	if ( sm.wheel && SM_InRect( x, y, w, h ) ) {
		list->scroll += sm.wheel;
		sm.wheel = 0;
	}
	list->scroll = Com_Clampi( 0, Q_max( count - rows, 0 ), list->scroll );
	list->sel = Com_Clampi( 0, Q_max( count - 1, 0 ), list->sel );

	if ( SM_Clicked( x, y, w, h ) ) {
		row = list->scroll + (int)( ( sm.clickY - y ) / SM_ROW_H );
		if ( row < count ) {
			list->sel = row;
			clicked = row;
		}
	}

	SM_Box( x, y, w, h, smPanelLight );
	for ( int i = 0; i < rows; i++ ) {
		float ry = y + i * SM_ROW_H;

		row = list->scroll + i;
		if ( row >= count )
			break;
		if ( row == list->sel )
			SM_Fill( x + 1, ry, w - 2, SM_ROW_H, smHighlight );
		else if ( SM_InRect( x, ry, w, SM_ROW_H ) )
			SM_Fill( x + 1, ry, w - 2, SM_ROW_H, smHover );
		drawRow( row, x + 4, ry + 1, w - 8 );
	}
	return clicked;
}

static void SM_DrawSelRow( int row, float x, float y, float w ) {
	const char *shader = sm.selShaders[row].c_str(), *remap = SM_RemappedTo( shader );

	if ( remap )
		SM_TextClipped( x, y, w, va( "%s " S_COLOR_CYAN "-> %s", shader, remap ), smWhite );
	else
		SM_TextClipped( x, y, w, shader, smWhite );
}

static void SM_DrawShaderRow( int row, float x, float y, float w ) {
	SM_TextClipped( x, y, w, sm.shaderNames[sm.shaderView[row]].c_str(), smWhite );
}

static void SM_DrawRemapRow( int row, float x, float y, float w ) {
	SM_TextClipped( x, y, w, va( "%s " S_COLOR_CYAN "-> %s", sm.remapFrom[row].c_str(), sm.remapTo[row].c_str() ), smWhite );
}

// a shader drawn flat in a box, on a checkerboard so see-through ones show
static void SM_DrawSwatch( float x, float y, float size, const char *label, const char *shader ) {
	qhandle_t h = shader ? SM_PreviewHandle( shader ) : 0;
	float cell = size / 8.0f;

	SM_Text( x, y, label, smDim );
	y += 12;
	SM_Box( x, y, size, size, smPanelDark );
	for ( int j = 0; j < 8; j++ ) {
		for ( int i = ( j & 1 ); i < 8; i += 2 )
			SM_Fill( x + i * cell, y + j * cell, cell, cell, smChecker );
	}
	if ( h ) {
		re->SetColor( NULL );
		re->DrawStretchPic( x + 1, y + 1, size - 2, size - 2, 0, 0, 1, 1, h );
	} else if ( shader ) {
		SM_Text( x + 4, y + size * 0.5f - 6, "No preview", smRed );
	}
}

// the readout box: under the crosshair, or under the mouse with the panel open
static void SM_DrawReadout( float cx, float cy ) {
	const char *lines[3], *remap;
	int numLines = 0;
	float w = 0.0f, h, x;

	switch ( sm.hit.kind ) {
	case SM_HIT_NONE:
		lines[numLines++] = S_COLOR_GREY "No surface";
		break;
	case SM_HIT_WORLD:
		lines[numLines++] = va( S_COLOR_YELLOW "%s", sm.hit.shader[0] ? sm.hit.shader : "(no shader)" );
		lines[numLines++] = S_COLOR_GREY "Map surface";
		break;
	case SM_HIT_BRUSH:
		lines[numLines++] = va( S_COLOR_YELLOW "%s", sm.hit.shader[0] ? sm.hit.shader : "(no shader)" );
		lines[numLines++] = va( S_COLOR_GREY "Brush entity %s", sm.hit.model );
		break;
	default:
		lines[numLines++] = va( S_COLOR_YELLOW "%s", sm.hit.shader[0] ? sm.hit.shader : "(no shader)" );
		lines[numLines++] = va( S_COLOR_GREY "%s%s%s", sm.hit.model, sm.hit.surface[0] ? "  surface " : "", sm.hit.surface );
		break;
	}
	remap = sm.hit.shader[0] ? SM_RemappedTo( sm.hit.shader ) : NULL;
	if ( remap )
		lines[numLines++] = va( S_COLOR_CYAN "remapped to %s", remap );

	for ( int i = 0; i < numLines; i++ )
		w = Q_max( w, SM_TextWidth( lines[i] ) );
	h = numLines * 12.0f + 6.0f;
	x = Com_Clamp( 8.0f, SCREEN_WIDTH - w - 8.0f, cx - w * 0.5f );

	SM_Fill( x - 6, cy - 3, w + 12, h, smPanel );
	for ( int i = 0; i < numLines; i++ )
		SM_Text( x, cy + i * 12.0f, lines[i], smWhite );
}

static void SM_DrawObjectTab( float x, float y, float w ) {
	const char *original, *remap, *replacement;
	int rows = SM_ShaderRows(), clicked;

	SM_TextClipped( x, y, w, va( S_COLOR_GREY "%s", sm.haveSel ? sm.selWhat : "Nothing selected: click something in the view" ), smWhite );
	y += 14;
	SM_Text( x, y, "Shaders on it", smDim );
	y += 12;
	if ( sm.haveSel ) {
		SM_List( &sm.selList, (int)sm.selShaders.size(), x, y, w, 5, SM_DrawSelRow );
	} else {
		SM_Box( x, y, w, 5 * SM_ROW_H, smPanelLight );
	}
	y += 5 * SM_ROW_H + 4;
	original = SM_SelectedOriginal();
	remap = original ? SM_RemappedTo( original ) : NULL;
	SM_TextClipped( x, y, w, remap ? va( S_COLOR_CYAN "Now remapped to %s", remap ) : original ? S_COLOR_GREY "Not remapped" : "", smWhite );
	y += 16;

	SM_Text( x, y + 2, "Replace with", smDim );
	{
		static const char *labels[] = { "All", "textures", "models", "gfx" };
		float bx = x + w - 4 * 50;

		for ( int i = 0; i < 4; i++ ) {
			if ( SM_Button( bx + i * 50, y, 48, labels[i], (qboolean)( sm.filter == i ), qtrue ) ) {
				sm.filter = (smFilter_t)i;
				sm.shaderList.scroll = 0;
				SM_RebuildShaderView();
			}
		}
	}
	y += 18;
	SM_Field( SM_F_SEARCH, x, y, w, va( "Type to search %i shaders", (int)sm.shaderNames.size() ) );
	y += 18;
	clicked = SM_List( &sm.shaderList, (int)sm.shaderView.size(), x, y, w, rows, SM_DrawShaderRow );
	if ( sm.shaderView.empty() )
		SM_Text( x + 4, y + 1, "No shaders match", smDim );
	(void)clicked;
	y += rows * SM_ROW_H + 4;
	replacement = SM_HighlightedShader();

	if ( sm.showPreview ) {
		SM_DrawSwatch( x, y, 72, "Current", original ? ( remap ? remap : original ) : NULL );
		SM_DrawSwatch( x + 84, y, 72, "New", replacement );
		SM_TextClipped( x + 168, y + 12, w - 168, S_COLOR_GREY "Flat preview;", smWhite );
		SM_TextClipped( x + 168, y + 24, w - 168, S_COLOR_GREY "Live shows it", smWhite );
		SM_TextClipped( x + 168, y + 36, w - 168, S_COLOR_GREY "on the object.", smWhite );
		y += 90;
	}

	SM_Text( x, y + 2, "Time offset", smDim );
	SM_Field( SM_F_OFFSET, x + 62, y, 40, "none" );
	if ( SM_Button( x + 108, y, 74, sm.livePreview ? "Live: on" : "Live: off", sm.livePreview, qtrue ) )
		sm.livePreview = (qboolean)!sm.livePreview;
	if ( SM_Button( x + 186, y, w - 186, sm.showPreview ? "Hide preview" : "Show preview", qfalse, qtrue ) )
		sm.showPreview = (qboolean)!sm.showPreview;
	y += 19;

	if ( SM_Button( x, y, 96, "Apply  (Enter)", qfalse, (qboolean)( original && replacement ) ) )
		SM_SendRemap( original, replacement, qtrue );
	if ( SM_Button( x + 100, y, 90, "Reset original", qfalse, (qboolean)( remap != NULL ) ) )
		SM_SendRemap( original, original, qtrue );
	if ( SM_Button( x + 194, y, w - 194, "Undo last", qfalse, (qboolean)!sm.undoShader.empty() ) )
		SM_Undo();

	// shown on this screen only, while it is being picked
	if ( sm.livePreview && original && replacement )
		SM_SetLive( original, replacement );
	else
		SM_ClearLive();
}

static void SM_DrawRemapsTab( float x, float y, float w ) {
	const char *from;
	int count = (int)sm.remapFrom.size();

	SM_ClearLive();
	SM_Text( x, y, va( S_COLOR_GREY "%i shader%s remapped in this map", count, count == 1 ? "" : "s" ), smWhite );
	y += 16;
	SM_List( &sm.remapList, count, x, y, w, 26, SM_DrawRemapRow );
	if ( !count )
		SM_Text( x + 4, y + 1, "None", smDim );
	y += 26 * SM_ROW_H + 6;
	from = sm.remapList.sel < count ? sm.remapFrom[sm.remapList.sel].c_str() : NULL;
	if ( SM_Button( x, y, 90, "Reset selected", qfalse, (qboolean)( from != NULL ) ) )
		SM_SendRemap( from, from, qtrue );
	if ( SM_Button( x + 94, y, 80, "Edit it...", qfalse, (qboolean)( from != NULL ) ) ) {
		SM_SelectShader( from );
		sm.tab = SM_TAB_OBJECT;
	}
	if ( SM_Button( x + 178, y, w - 178, "Undo last", qfalse, (qboolean)!sm.undoShader.empty() ) )
		SM_Undo();
}

static void SM_DrawPanel( void ) {
	const float px = SM_PANEL_X, pw = SM_PANEL_W, x = px + 8, w = pw - 16;
	qboolean click;

	SM_Box( px, 8, pw, SCREEN_HEIGHT - 16, smPanelDark );
	SM_Text( x, 14, "Shader Manager", smWhite );
	if ( SM_Button( x + w - 120, 12, 58, "Object", (qboolean)( sm.tab == SM_TAB_OBJECT ), qtrue ) )
		sm.tab = SM_TAB_OBJECT;
	if ( SM_Button( x + w - 58, 12, 58, "Remaps", (qboolean)( sm.tab == SM_TAB_REMAPS ), qtrue ) )
		sm.tab = SM_TAB_REMAPS;

	if ( sm.tab == SM_TAB_OBJECT )
		SM_DrawObjectTab( x, 34, w );
	else
		SM_DrawRemapsTab( x, 34, w );

	// the server's answer, else what was sent
	if ( sm.reply[0] && cls.realtime - sm.replyTime < 8000 )
		SM_TextClipped( x, SCREEN_HEIGHT - 24, w, va( S_COLOR_YELLOW "Server: " S_COLOR_WHITE "%s", sm.reply ), smWhite );
	else if ( sm.status[0] )
		SM_TextClipped( x, SCREEN_HEIGHT - 24, w, va( "%s%s", cls.realtime - sm.sentTime < 1500 ? S_COLOR_GREEN : S_COLOR_GREY, sm.status ), smWhite );

	// a click in the view picks what is under it
	click = sm.click;
	if ( click && sm.clickX < SM_PANEL_X ) {
		sm.click = qfalse;
		sm.focus = SM_F_NONE;
		SM_SelectUnderRay();
	}
}

void CL_ShaderManager_Draw( void ) {
	if ( !sm.initialized )
		return;
	if ( Key_GetCatcher() & ( KEYCATCH_UI | KEYCATCH_NPCMANAGER ) )
		return;
	if ( sm.state == SM_OFF && !cl_shaderCrosshair->integer )
		return;
	if ( !sm.font ) {
		sm.font = re->RegisterFont( "arialnb" );
		if ( !sm.font )
			sm.font = cls.menuFont;
	}

	SM_UpdateHit();
	if ( sm.state == SM_OFF ) {
		if ( sm.hitValid )
			SM_DrawReadout( SCREEN_WIDTH * 0.5f, SCREEN_HEIGHT * 0.5f + 24.0f );
		return;
	}

	if ( sm.state == SM_CAMERA ) {
		if ( sm.click ) {
			sm.click = qfalse;
			SM_SelectUnderRay();
			if ( sm.state == SM_EDIT )
				return;
		}
		SM_Fill( SCREEN_WIDTH * 0.5f - 1, SCREEN_HEIGHT * 0.5f - 6, 2, 12, smWhite );
		SM_Fill( SCREEN_WIDTH * 0.5f - 6, SCREEN_HEIGHT * 0.5f - 1, 12, 2, smWhite );
		if ( sm.hitValid )
			SM_DrawReadout( SCREEN_WIDTH * 0.5f, SCREEN_HEIGHT * 0.5f + 24.0f );
		SM_Text( 16, SCREEN_HEIGHT - 28, S_COLOR_GREY "WASD fly   Space/C up/down   Shift faster   Ctrl slower", smWhite );
		SM_Text( 16, SCREEN_HEIGHT - 16, S_COLOR_GREY "Click: pick what's under the crosshair   Tab: remap list   Esc: close", smWhite );
		return;
	}

	SM_DrawPanel();
	if ( sm.hitValid )
		SM_DrawReadout( sm.cursorX, sm.cursorY + 20.0f );
	SM_Text( 16, SCREEN_HEIGHT - 28, S_COLOR_GREY "Hold right mouse: look and fly (WASD)", smWhite );
	SM_Text( 16, SCREEN_HEIGHT - 16, S_COLOR_GREY "Click in the view: pick   Tab: tabs   Esc: back", smWhite );

	// a click on nothing leaves the text box
	if ( sm.click )
		sm.focus = SM_F_NONE;
	sm.click = qfalse;
	sm.wheel = 0;
	if ( !sm.looking )
		re->DrawStretchPic( sm.cursorX, sm.cursorY, 32, 32, 0, 0, 1, 1, cls.cursorShader );
}

/*
===============================================================================

SETUP

===============================================================================
*/

void CL_ShaderManager_f( void ) {
	const char *arg = Cmd_Argv( 1 );

	if ( Cmd_Argc() < 2 ) {
		if ( sm.state != SM_OFF )
			CL_ShaderManager_Close();
		else
			SM_Open();
	} else if ( !Q_stricmp( arg, "crosshair" ) ) {
		Cvar_SetValue( "cl_shaderCrosshair", cl_shaderCrosshair->integer ? 0 : 1 );
		Com_Printf( "Shader under the crosshair: %s\n", cl_shaderCrosshair->integer ? "shown" : "hidden" );
	} else if ( !Q_stricmp( arg, "remaps" ) ) {
		if ( sm.remapFrom.empty() )
			Com_Printf( "No shaders are remapped\n" );
		for ( size_t i = 0; i < sm.remapFrom.size(); i++ )
			Com_Printf( "%s -> %s\n", sm.remapFrom[i].c_str(), sm.remapTo[i].c_str() );
	} else if ( !Q_stricmp( arg, "reindex" ) ) {
		SM_BuildIndex();
		SM_RebuildShaderView();
	} else if ( !Q_stricmp( arg, "close" ) ) {
		CL_ShaderManager_Close();
	} else {
		Com_Printf( "usage: " SM_TOGGLE_CMD "              toggle the shader manager (bind a key to it)\n" );
		Com_Printf( "       " SM_TOGGLE_CMD " crosshair    show/hide the shader under the crosshair\n" );
		Com_Printf( "       " SM_TOGGLE_CMD " remaps       list the shaders remapped in this map\n" );
		Com_Printf( "       " SM_TOGGLE_CMD " reindex      rescan the filesystem for shaders\n" );
		Com_Printf( "       " SM_TOGGLE_CMD " close        close the shader manager\n" );
	}
}

void CL_ShaderManager_Init( void ) {
	cl_shaderCrosshair = Cvar_Get( "cl_shaderCrosshair", "0", CVAR_ARCHIVE, "Show the shader under the crosshair" );
	sm.initialized = qtrue;
	sm.showPreview = qtrue;
	sm.livePreview = qtrue;
	sm.selSurf = -1;
}

// cgame is going away: map change, disconnect or vid_restart
void CL_ShaderManager_Shutdown( void ) {
	CL_ShaderManager_Close();
	SM_FreeMap();
	for ( int i = 0; i < SM_MAX_HANDLES; i++ ) {
		sm.modelNames[i].clear();
		sm.skinNames[i].clear();
		sm.md3s[i] = smMd3_t();
		sm.skins[i] = smSkin_t();
	}
	sm.glmNames.clear();
	sm.glms.clear();
	sm.remapFrom.clear();
	sm.remapTo.clear();
	sm.undoShader.clear();
	sm.undoPrevious.clear();
	sm.haveSel = qfalse;
	sm.selShaders.clear();
	sm.haveView = qfalse;
	sm.hitValid = qfalse;
	sm.numPending = sm.numEntities = 0;
	sm.font = 0;
	// the pure pk3 list can change with the next map or server, and the renderer drops its shaders
	sm.indexed = qfalse;
	std::vector<std::string>().swap( sm.shaderNames );
	sm.previews.clear();
	sm.shaderView.clear();
}
