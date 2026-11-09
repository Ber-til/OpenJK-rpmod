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

// cl_sabers.cpp -- a saber as ext_data/sabers/*.sab has it: what it's called, its hilt model and
// its type. Presets, the NPC manager and the inventory's hilt all read them here.

#include "client.h"

// the first block with the saber's name, in any .sab: qfalse if none has it
qboolean CL_Saber_Find( const char *saber, clSaber_t *out ) {
	char **files, *buf, key[64];
	const char *p, *token;
	int numFiles;
	qboolean found = qfalse;

	memset( out, 0, sizeof( *out ) );
	if ( !saber || !saber[0] || !Q_stricmp( saber, "none" ) )
		return qfalse;
	files = FS_ListFiles( "ext_data/sabers", ".sab", &numFiles );
	for ( int i = 0; i < numFiles && !found; i++ ) {
		const char *path = va( "ext_data/sabers/%s", files[i] );

		if ( FS_ReadFile( path, (void **)&buf ) <= 0 || !buf )
			continue;
		p = buf;
		COM_BeginParseSession( path );
		while ( !found ) {
			char name[MAX_QPATH];

			token = COM_ParseExt( &p, qtrue );
			if ( !token[0] )
				break;
			Q_strncpyz( name, token, sizeof( name ) );
			token = COM_ParseExt( &p, qtrue );
			if ( strcmp( token, "{" ) ) {
				if ( !token[0] )
					break;
				continue;
			}
			if ( Q_stricmp( name, saber ) ) {
				SkipBracedSection( &p, 1 );
				continue;
			}
			// its block: the keys wanted
			found = qtrue;
			Q_strncpyz( out->id, name, sizeof( out->id ) );
			while ( 1 ) {
				token = COM_ParseExt( &p, qtrue );
				if ( !token[0] || !strcmp( token, "}" ) )
					break;
				if ( !strcmp( token, "{" ) ) {
					SkipBracedSection( &p, 1 );
					continue;
				}
				Q_strncpyz( key, token, sizeof( key ) );
				token = COM_ParseExt( &p, qfalse );
				if ( !Q_stricmp( key, "saberModel" ) && token[0] )
					Q_strncpyz( out->model, token, sizeof( out->model ) );
				else if ( !Q_stricmp( key, "name" ) && token[0] )
					Q_strncpyz( out->name, token, sizeof( out->name ) );
				else if ( !Q_stricmp( key, "saberType" ) && token[0] )
					Q_strncpyz( out->type, token, sizeof( out->type ) );
				if ( token[0] )
					SkipRestOfLine( &p );
			}
		}
		FS_FreeFile( buf );
	}
	FS_FreeFileList( files );
	if ( found && !out->name[0] )
		Q_strncpyz( out->name, out->id, sizeof( out->name ) );
	return found;
}
