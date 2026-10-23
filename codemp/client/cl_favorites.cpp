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

// cl_favorites.cpp -- the managers' favorites
//
// A star on a row of the modelmanager's, effectmanager's or shadermanager's lists marks a
// model, effect or shader as a favorite, to find it again among thousands. Each list is
// kept in favorites/<list>.txt, a name a line, compared without case.

#include <map>
#include <set>
#include <string>
#include "client.h"

#define FAV_DIR		"favorites"

typedef std::set<std::string> favSet_t;

static std::map<std::string, favSet_t> favLists;

static std::string Fav_Key( const char *name ) {
	std::string key = name;

	for ( size_t i = 0; i < key.size(); i++ ) {
		key[i] = (char)tolower( (unsigned char)key[i] );
		if ( key[i] == '\\' )
			key[i] = '/';
	}
	return key;
}

// the list, read the first time it is asked for
static favSet_t &Fav_List( const char *list ) {
	std::map<std::string, favSet_t>::iterator it = favLists.find( list );
	char *buf;

	if ( it != favLists.end() )
		return it->second;
	favSet_t &set = favLists[list];
	if ( FS_ReadFile( va( FAV_DIR "/%s.txt", list ), (void **)&buf ) > 0 && buf ) {
		for ( char *line = buf, *next; line && *line; line = next ) {
			next = strchr( line, '\n' );
			if ( next )
				*next++ = '\0';
			for ( char *end = line + strlen( line ); end > line && ( end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t' ); )
				*--end = '\0';
			while ( *line == ' ' || *line == '\t' )
				line++;
			if ( line[0] && strncmp( line, "//", 2 ) )
				set.insert( Fav_Key( line ) );
		}
		FS_FreeFile( buf );
	}
	return set;
}

static void Fav_Save( const char *list ) {
	const favSet_t &set = Fav_List( list );
	std::string out = "// the managers' favorites, a name a line\n";

	for ( favSet_t::const_iterator it = set.begin(); it != set.end(); ++it )
		out += *it + "\n";
	FS_WriteFile( va( FAV_DIR "/%s.txt", list ), out.c_str(), (int)out.size() );
}

qboolean CL_Fav_Has( const char *list, const char *name ) {
	const favSet_t &set = Fav_List( list );

	if ( !name || !name[0] || set.empty() )
		return qfalse;
	return (qboolean)( set.find( Fav_Key( name ) ) != set.end() );
}

// a favorite now, or not any more; returns which
qboolean CL_Fav_Toggle( const char *list, const char *name ) {
	favSet_t &set = Fav_List( list );
	const std::string key = Fav_Key( name );
	qboolean on;

	if ( !name || !name[0] )
		return qfalse;
	if ( set.erase( key ) ) {
		on = qfalse;
	} else {
		set.insert( key );
		on = qtrue;
	}
	Fav_Save( list );
	return on;
}

int CL_Fav_Count( const char *list ) {
	return (int)Fav_List( list ).size();
}

// the star a row starts with: lit for a favorite
const char *CL_Fav_Star( qboolean on ) {
	return on ? S_COLOR_YELLOW "*" : S_COLOR_GREY "*";
}
