/*
===========================================================================
Copyright (C) 2026 OpenJK-rpmod contributors

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

// cl_chat.cpp -- the datapad's Chat: instant messages between characters
//
// You, as your account's main character, to another RPMod account's main character (never your own),
// through C1B3R (c1b3r/chat.py), which keeps the messages and their history. One sent reaches the
// other's game at once on whatever server it's on (cmd:chat/<client>/<id>: <account>|<name>|<text>):
// with the conversation open it's there with its sound; otherwise a notice as whispers have
// (RPMod's info banner) and a ringtone. Both sounds are picked in the sound manager, in Settings.
//
// Live, as a phone's messenger: yours are marked sent, delivered (their game has it) and seen (they
// opened the conversation), as C1B3R tells (cmd:chatstate/<client>: <account>|<delivered up to
// id>|<seen up to id>|<seen time>); typing tells the other's game every few seconds while it goes on
// (/v2/chat/typing), shown as "<name> is typing..." (cmd:chattyping/<client>: <account>).

#include "client.h"
#include "cl_datapad.h"
#include "cl_presence.h"
#include "cl_widgets.h"
#include <string>
#include <vector>

#define CH_TEXT_LEN		500
#define CH_ROW_H		26.0f
#define CH_REFRESH_MS	20000		// the contacts asked again this often while they're shown
#define CH_DEFAULT_RING	"sound/effects/info"			// RPMod's info sound, as whispers have
#define CH_DEFAULT_SOUND	"sound/interface/sub_select"
#define CH_TYPING_EVERY	3000		// typing told again this often while it goes on
#define CH_TYPING_SHOWN	6000		// their typing shown this long after the last word of it

typedef enum { CH_LIST, CH_TALK, CH_SETTINGS } chView_t;

typedef struct {
	std::string	account, name;
	qboolean	online;
	int			unread;
	int			last;
} chContact_t;

typedef struct {
	int			id;
	qboolean	mine;
	int			time;
	std::string	text;
	int			state;			// yours: 0 sent, 1 delivered, 2 seen; -1 not known (C1B3R from before)
	int			seenAt;
} chMessage_t;

static struct {
	qboolean		shown;
	chView_t		view;
	std::vector<chContact_t>	contacts;
	int				totalUnread;
	qboolean		known;			// the contacts asked once
	int				contactsAt;
	int				listScroll;

	std::string		with, withName;	// the conversation open
	std::vector<chMessage_t>	messages;
	qboolean		loading;
	float			talkScroll;		// from the bottom: 0 the latest
	char			text[CH_TEXT_LEN];
	qboolean		typing;
	qboolean		historyAgain;	// asked while the last ask was still going
	int				typingSentAt;	// yours told
	int				theyTypeUntil;	// theirs shown until

	int				picking;		// a sound being picked in the sound manager: 1 the message sound, 2 the ringtone
	qboolean		picked;

	char			message[160];
	int				messageTime;
} ch;

static httpRequest_t chContactsHttp, chHistoryHttp, chSendHttp, chTypingHttp;
static cvar_t *cl_chatNotify, *cl_chatRingtone, *cl_chatSound;

static void CH_Message( const char *text ) {
	Q_strncpyz( ch.message, text, sizeof( ch.message ) );
	ch.messageTime = cls.realtime;
}

static qboolean CH_Post( httpRequest_t *req, const char *path, const std::string &fields ) {
	const char *session = CL_Presence_Session();
	std::string body = "{\"session\":";

	if ( !session || req->state == HTTP_BUSY )
		return qfalse;
	CL_Presence_Json( body, session );
	body += fields + "}";
	return CL_Presence_Post( req, path, body );
}

// a sound of the settings', played here alone
static void CH_Play( const char *path ) {
	sfxHandle_t sfx;

	if ( !path || !path[0] )
		return;
	sfx = S_RegisterSound( path );
	if ( sfx )
		S_StartLocalSound( sfx, CHAN_LOCAL_SOUND );
}

static void CH_AskContacts( void ) {
	if ( CH_Post( &chContactsHttp, "/v2/chat/contacts", "" ) ) {
		ch.known = qtrue;
		ch.contactsAt = cls.realtime;
	}
}

static void CH_AskHistory( void ) {
	std::string f = ",\"with\":";

	CL_Presence_Json( f, ch.with.c_str() );
	ch.historyAgain = (qboolean)( !CH_Post( &chHistoryHttp, "/v2/chat/history", f ) && chHistoryHttp.state == HTTP_BUSY );
}

// typing in the conversation: the other's game told, again every few seconds while it goes on
static void CH_Typed( void ) {
	std::string f = ",\"to\":";

	if ( !ch.text[0] || ch.with.empty() || ( ch.typingSentAt && cls.realtime - ch.typingSentAt < CH_TYPING_EVERY ) )
		return;
	CL_Presence_Json( f, ch.with.c_str() );
	if ( CH_Post( &chTypingHttp, "/v2/chat/typing", f ) )
		ch.typingSentAt = cls.realtime;
}

static void CH_Open( const chContact_t &c ) {
	ch.view = CH_TALK;
	ch.with = c.account;
	ch.withName = c.name;
	ch.messages.clear();
	ch.loading = qtrue;
	ch.talkScroll = 0;
	ch.typing = qtrue;
	ch.theyTypeUntil = 0;
	ch.typingSentAt = 0;
	WG_EditReset();
	CH_AskHistory();
}

// datapad chat <account>: straight to that conversation
void CL_Chat_OpenWith( const char *account ) {
	chContact_t c;

	if ( !account || !account[0] )
		return;
	c.account = account;
	c.name = account;			// until C1B3R says whose it is
	for ( size_t i = 0; i < ch.contacts.size(); i++ ) {
		if ( !Q_stricmp( ch.contacts[i].account.c_str(), account ) ) {
			c = ch.contacts[i];
			break;
		}
	}
	for ( size_t i = 0; i < c.account.size(); i++ )
		c.account[i] = (char)tolower( (unsigned char)c.account[i] );
	CH_Open( c );
}

static void CH_Send( void ) {
	std::string f = ",\"to\":";

	if ( !ch.text[0] || ch.with.empty() )
		return;
	CL_Presence_Json( f, ch.with.c_str() );
	f += ",\"text\":";
	CL_Presence_Json( f, ch.text );
	if ( !CH_Post( &chSendHttp, "/v2/chat/send", f ) ) {
		CH_Message( CL_Presence_Session() ? "A moment: the last one's still going" : "Not connected to C1B3R" );
		return;
	}
	// shown at once, as sent
	{
		chMessage_t m;

		m.id = 0;
		m.mine = qtrue;
		m.time = 0;
		m.text = ch.text;
		m.state = -1;
		m.seenAt = 0;
		ch.messages.push_back( m );
	}
	ch.text[0] = '\0';
	ch.typingSentAt = 0;		// the next words typed are told at once
	ch.talkScroll = 0;
	WG_EditReset();
	CH_Play( cl_chatSound->string );
}

// the lines of an answer, split on tabs, each read back as it was sent
static void CH_Lines( const char *answer, void ( *each )( const std::vector<std::string> & ) ) {
	std::string all( answer ), line;
	size_t pos = 0;

	while ( pos < all.size() ) {
		size_t end = all.find( '\n', pos ), at = 0;
		std::vector<std::string> f;

		if ( end == std::string::npos )
			end = all.size();
		line = all.substr( pos, end - pos );
		pos = end + 1;
		while ( at <= line.size() ) {
			size_t tab = line.find( '\t', at );

			if ( tab == std::string::npos )
				tab = line.size();
			f.push_back( CL_Presence_Unfield( line.substr( at, tab - at ).c_str() ) );
			at = tab + 1;
		}
		each( f );
	}
}

static void CH_ReadContact( const std::vector<std::string> &f ) {
	if ( f.size() >= 2 && f[0] == "ok" ) {
		ch.totalUnread = atoi( f[1].c_str() );
	} else if ( f.size() >= 6 && f[0] == "contact" ) {
		chContact_t c;

		c.account = f[1];
		c.name = f[2];
		c.online = (qboolean)( f[3] == "1" );
		c.unread = atoi( f[4].c_str() );
		c.last = atoi( f[5].c_str() );
		ch.contacts.push_back( c );
	}
}

static void CH_ReadMessage( const std::vector<std::string> &f ) {
	if ( f.size() >= 3 && f[0] == "ok" ) {
		ch.withName = f[2];
	} else if ( f.size() >= 5 && f[0] == "msg" ) {
		chMessage_t m;

		m.id = atoi( f[1].c_str() );
		m.mine = (qboolean)( f[2] == "1" );
		m.time = atoi( f[3].c_str() );
		m.text = f[4];
		m.state = f.size() >= 6 && m.mine ? atoi( f[5].c_str() ) : -1;
		m.seenAt = f.size() >= 7 ? atoi( f[6].c_str() ) : 0;
		ch.messages.push_back( m );
	}
}

/*
===============================================================================

A message come: the conversation, or a notice and its ringtone

===============================================================================
*/

// cmd:chat/<client>/<id>: <from account>|<from name>|<the text's start>
void CL_Chat_Hook( char **argv, int argc, const char *message ) {
	char account[64] = "", name[MAX_QPATH] = "", text[256] = "";
	const char *p = message ? message : "", *bar;

	if ( argc < 1 || atoi( argv[0] ) != clc.clientNum )
		return;
	if ( ( bar = strchr( p, '|' ) ) != NULL ) {
		Q_strncpyz( account, p, Q_min( (int)sizeof( account ), (int)( bar - p ) + 1 ) );
		p = bar + 1;
		if ( ( bar = strchr( p, '|' ) ) != NULL ) {
			Q_strncpyz( name, p, Q_min( (int)sizeof( name ), (int)( bar - p ) + 1 ) );
			Q_strncpyz( text, bar + 1, sizeof( text ) );
		}
	}
	// the conversation open on the datapad: there, with its sound
	if ( ch.shown && ch.view == CH_TALK && !Q_stricmp( ch.with.c_str(), account ) ) {
		ch.theyTypeUntil = 0;
		CH_AskHistory();
		CH_Play( cl_chatSound->string );
		return;
	}
	ch.totalUnread++;
	if ( ch.shown && ch.view == CH_LIST )
		CH_AskContacts();
	if ( !cl_chatNotify->integer || cls.state != CA_ACTIVE )
		return;
	// a notice as whispers have, the same for every message (who and what are on the datapad), and the ringtone
	CL_QueueCgameCommand( "info 5000 2 0 8 \":: You have a new message on your datapad ::\"" );
	Com_Printf( ":: You have a new message on your datapad ::\n" );
	CH_Play( cl_chatRingtone->string );
}

// the account at the start of a hook's message, up to the first bar
static void CH_HookAccount( const char *message, char *account, int size, const char **rest ) {
	const char *p = message ? message : "", *bar = strchr( p, '|' );

	while ( *p == ' ' )
		p++;
	Q_strncpyz( account, p, bar ? Q_min( size, (int)( bar - p ) + 1 ) : size );
	*rest = bar ? bar + 1 : "";
}

// cmd:chatstate/<client>: <account>|<delivered up to id>|<seen up to id>|<seen time>
void CL_Chat_StateHook( char **argv, int argc, const char *message ) {
	char account[64];
	const char *rest;
	int delivered, seen, seenAt;

	if ( argc < 1 || atoi( argv[0] ) != clc.clientNum )
		return;
	CH_HookAccount( message, account, sizeof( account ), &rest );
	if ( Q_stricmp( ch.with.c_str(), account ) )
		return;
	delivered = atoi( rest );
	rest = strchr( rest, '|' ) ? strchr( rest, '|' ) + 1 : "";
	seen = atoi( rest );
	rest = strchr( rest, '|' ) ? strchr( rest, '|' ) + 1 : "";
	seenAt = atoi( rest );
	for ( size_t i = 0; i < ch.messages.size(); i++ ) {
		chMessage_t &m = ch.messages[i];

		if ( !m.mine || m.id <= 0 )
			continue;
		if ( m.id <= seen ) {
			m.state = 2;
			m.seenAt = seenAt;
		} else if ( m.id <= delivered && m.state < 1 ) {
			m.state = 1;
		}
	}
}

// cmd:chattyping/<client>: <account>
void CL_Chat_TypingHook( char **argv, int argc, const char *message ) {
	char account[64];
	const char *rest;

	if ( argc < 1 || atoi( argv[0] ) != clc.clientNum )
		return;
	CH_HookAccount( message, account, sizeof( account ), &rest );
	if ( ch.view == CH_TALK && !Q_stricmp( ch.with.c_str(), account ) )
		ch.theyTypeUntil = cls.realtime + CH_TYPING_SHOWN;
}

/*
===============================================================================

Picking a sound in the sound manager

===============================================================================
*/

static void CH_Pick( int which ) {
	ch.picking = which;
	ch.picked = qfalse;
	CL_Datapad_Close();
	CL_SoundManager_OpenForChat();
	if ( !CL_SoundManager_Active() ) {
		ch.picking = 0;
		Cbuf_AddText( "datapad chat\n" );
	}
}

// the sound manager about to send: the sound is the chat's instead, nothing goes out
qboolean CL_Chat_CaptureSound( const char *cmd ) {
	char buf[MAX_STRING_CHARS], *argv[6];
	int argc = 0;

	if ( !ch.picking || !CL_SoundManager_Active() )
		return qfalse;
	if ( ch.picked || Q_stricmpn( cmd, "rpsound ", 8 ) )
		return qtrue;
	Q_strncpyz( buf, cmd, sizeof( buf ) );
	for ( char *p = buf; *p && argc < (int)ARRAY_LEN( argv ); ) {
		while ( *p == ' ' )
			*p++ = '\0';
		if ( !*p )
			break;
		argv[argc++] = p;
		while ( *p && *p != ' ' )
			p++;
	}
	if ( argc < 3 )
		return qtrue;
	Cvar_Set( ch.picking == 2 ? "cl_chatRingtone" : "cl_chatSound", !Q_stricmpn( argv[2], "sound/", 6 ) ? argv[2] : va( "sound/%s", argv[2] ) );
	ch.picked = qtrue;
	return qtrue;
}

// back from the sound manager once a sound's picked, or it was closed
static void CH_PickFrame( void ) {
	if ( !ch.picking || ( CL_SoundManager_Active() && !ch.picked ) )
		return;
	if ( CL_SoundManager_Active() )
		CL_SoundManager_Close();
	if ( ch.picked )
		CH_Play( ch.picking == 2 ? cl_chatRingtone->string : cl_chatSound->string );
	ch.picking = 0;
	ch.view = CH_SETTINGS;
	Cbuf_AddText( "datapad chat\n" );
}

/*
===============================================================================

Each frame, and the datapad's

===============================================================================
*/

void CL_Chat_Frame( void ) {
	if ( HTTP_Poll( &chContactsHttp ) ) {
		if ( chContactsHttp.status == 200 && !strncmp( chContactsHttp.answer, "ok", 2 ) ) {
			ch.contacts.clear();
			CH_Lines( chContactsHttp.answer, CH_ReadContact );
		} else if ( chContactsHttp.status != 200 && chContactsHttp.answer[0] ) {
			CH_Message( chContactsHttp.answer );
		}
	}
	if ( HTTP_Poll( &chHistoryHttp ) ) {
		ch.loading = qfalse;
		if ( chHistoryHttp.status == 200 && !strncmp( chHistoryHttp.answer, "ok", 2 ) ) {
			ch.messages.clear();
			CH_Lines( chHistoryHttp.answer, CH_ReadMessage );
		} else if ( chHistoryHttp.answer[0] ) {
			CH_Message( chHistoryHttp.answer );
		}
		// a message came while that was going: asked again, so it's there
		if ( ch.historyAgain && ch.view == CH_TALK && !ch.with.empty() ) {
			ch.historyAgain = qfalse;
			CH_AskHistory();
		}
	}
	HTTP_Poll( &chTypingHttp );
	if ( HTTP_Poll( &chSendHttp ) ) {
		if ( chSendHttp.status == 200 && !strncmp( chSendHttp.answer, "ok\t", 3 ) ) {
			// ok <id> <time> [<state>]: the one shown as sending is sent
			char buf[64], *f[4];
			int n = 0;

			Q_strncpyz( buf, chSendHttp.answer, sizeof( buf ) );
			for ( char *p = buf; n < 4; ) {
				f[n++] = p;
				p = strpbrk( p, "\t\r\n" );
				if ( !p )
					break;
				*p++ = '\0';
			}
			for ( size_t i = 0; n >= 3 && i < ch.messages.size(); i++ ) {
				chMessage_t &m = ch.messages[i];

				if ( m.mine && m.id == 0 ) {
					m.id = atoi( f[1] );
					m.time = atoi( f[2] );
					m.state = n >= 4 ? atoi( f[3] ) : -1;
					break;
				}
			}
		} else if ( chSendHttp.status != 200 ) {
			CH_Message( chSendHttp.answer[0] ? chSendHttp.answer : "Not sent: C1B3R didn't answer" );
			if ( ch.view == CH_TALK )
				CH_AskHistory();		// what was kept, without the one that wasn't
		}
	}
	CH_PickFrame();
	if ( ch.shown && ch.view == CH_LIST && ( !ch.known || cls.realtime - ch.contactsAt > CH_REFRESH_MS ) && CL_Presence_Session() )
		CH_AskContacts();
}

void CL_Chat_Shown( qboolean shown ) {
	ch.shown = shown;
	ch.theyTypeUntil = 0;
	if ( shown ) {
		if ( ch.view == CH_TALK && !ch.with.empty() )
			CH_AskHistory();
		else
			CH_AskContacts();
	}
}

int CL_Chat_Unread( void ) {
	return ch.totalUnread;
}

/*
===============================================================================

Drawing

===============================================================================
*/

static void CH_Style( wgStyle_t *style ) {
	memset( style, 0, sizeof( *style ) );
	style->font = DP_Font();
	style->scale = 0.62f;
	style->lineH = 12;
	style->padX = 3;
	style->padY = 2;
	style->fill = dpScreen;
	style->border = dpGlow;
	style->text = dpText;
	style->dim = dpDim;
}

static void CH_DrawList( float x, float y, float w, float bottom ) {
	float cx, cy;
	qboolean *click;
	const int visible = Q_max( 1, (int)( ( bottom - y ) / CH_ROW_H ) );
	const int num = (int)ch.contacts.size();
	qboolean bar;
	float rw;

	if ( !num ) {
		DP_Text( x, y + 30, w, !CL_Presence_Session() ? "Not connected to C1B3R" : chContactsHttp.state == HTTP_BUSY || !ch.known
			? "Asking C1B3R..." : "No one to talk with yet", dpDim, DP_Font(), 0.62f, qtrue );
		return;
	}
	click = DP_Click( &cx, &cy );
	bar = WG_Scrollbar( x + w - WG_SCROLL_W, y, visible * CH_ROW_H, num, visible, &ch.listScroll, DP_CursorX(), DP_CursorY(), click, cx, cy );
	rw = w - ( bar ? WG_SCROLL_W + 2 : 0 );
	ch.listScroll = Com_Clampi( 0, Q_max( 0, num - visible ), ch.listScroll );
	for ( int i = ch.listScroll; i < num && i < ch.listScroll + visible; i++ ) {
		const chContact_t &c = ch.contacts[i];
		const float ry = y + ( i - ch.listScroll ) * CH_ROW_H;
		const qboolean hover = DP_InRect( x, ry, rw, CH_ROW_H - 2 );

		DP_Fill( x, ry, rw, CH_ROW_H - 2, hover ? dpButtonHover : dpButton );
		DP_Frame( x, ry, rw, CH_ROW_H - 2, dpGlow );
		// online: a light by the name
		DP_Fill( x + 4, ry + 6, 5, 5, c.online ? dpWhite : dpDim );
		DP_Text( x + 13, ry + 2, rw * 0.7f, c.name.c_str(), hover ? dpWhite : dpText, DP_Font(), 0.62f, qfalse );
		DP_Text( x + 13, ry + 12, rw * 0.7f, c.last > 0 ? DP_JediStamp( c.last ) : c.account.c_str(), dpDim, DP_Font(), 0.5f, qfalse );
		if ( c.unread > 0 )
			DP_Text( x + rw * 0.72f, ry + 6, rw * 0.28f - 6, va( "^2%i new", c.unread ), dpWhite, DP_Font(), 0.58f, qfalse );
		if ( DP_Clicked( x, ry, rw, CH_ROW_H - 2 ) ) {
			ch.totalUnread = Q_max( 0, ch.totalUnread - c.unread );
			CH_Open( c );
			return;
		}
	}
}

// a message's line above its text: when, and for yours how far it got (seen at what time)
static const char *CH_Stamp( const chMessage_t &m ) {
	static char line[96];
	char when[48], seen[48] = "";
	const char *at;

	if ( m.time <= 0 )
		return "sending...";
	Q_strncpyz( when, DP_JediStamp( m.time ), sizeof( when ) );
	if ( !m.mine || m.state < 0 )
		return va( "%s", when );
	if ( m.state == 2 && m.seenAt > 0 && ( at = strchr( DP_JediStamp( m.seenAt ), '@' ) ) != NULL )
		Q_strncpyz( seen, at, sizeof( seen ) );
	Com_sprintf( line, sizeof( line ), "%s  -  %s%s%s", when, m.state == 2 ? "seen" : m.state == 1 ? "delivered" : "sent",
		seen[0] ? " " : "", seen );
	return line;
}

static void CH_DrawTalk( float x, float y, float w, float bottom ) {
	wgStyle_t style;
	const float inputY = bottom - DP_CTRL_H, top = y;
	float my;
	float cx, cy;
	qboolean *click;

	CH_Style( &style );
	// the messages, the latest at the bottom: theirs at the left, yours at the right
	if ( ch.loading && ch.messages.empty() ) {
		DP_Text( x, y + 30, w, "Asking C1B3R...", dpDim, DP_Font(), 0.62f, qtrue );
	} else if ( ch.messages.empty() ) {
		DP_Text( x, y + 30, w, "No messages yet: say hello", dpDim, DP_Font(), 0.62f, qtrue );
	} else {
		const qboolean theyType = (qboolean)( ch.theyTypeUntil > cls.realtime );

		my = inputY - ( theyType ? 18 : 6 ) + ch.talkScroll;
		for ( int i = (int)ch.messages.size() - 1; i >= 0 && my > top; i-- ) {
			const chMessage_t &m = ch.messages[i];
			const float bw = w * 0.78f, bx = m.mine ? x + w - bw : x;
			const int lines = Q_max( 1, (int)( m.text.size() / 52 ) + 1 );
			const float bh = 13.0f + lines * 10.0f;

			my -= bh + 3;
			if ( my + bh < top || my > inputY )
				continue;
			if ( my >= top ) {
				DP_Fill( bx, my, bw, bh, m.mine ? dpButtonHover : dpButton );
				DP_Frame( bx, my, bw, bh, m.mine ? dpWhite : dpGlow );
				DP_Text( bx + 4, my + 1, bw - 8, CH_Stamp( m ), m.mine && m.state == 2 ? dpGlow : dpDim, DP_Font(), 0.48f, qfalse );
				// the text, wrapped on spaces at about the box's width
				{
					std::string rest = m.text;
					float ly = my + 11;

					while ( !rest.empty() && ly < my + bh ) {
						size_t cut = rest.size() <= 52 ? rest.size() : rest.rfind( ' ', 52 );

						if ( cut == std::string::npos || cut == 0 )
							cut = Q_min( (size_t)52, rest.size() );
						DP_Text( bx + 4, ly, bw - 8, rest.substr( 0, cut ).c_str(), dpText, DP_Font(), 0.56f, qfalse );
						rest.erase( 0, cut );
						while ( !rest.empty() && rest[0] == ' ' )
							rest.erase( 0, 1 );
						ly += 10;
					}
				}
			}
		}
	}
	if ( ch.theyTypeUntil > cls.realtime )
		DP_Text( x + 2, inputY - 14, w, va( "%s is typing...", ch.withName.c_str() ), dpDim, DP_Font(), 0.5f, qfalse );
	// what you type, Enter sends it
	DP_Fill( x, inputY, w - DP_Wide( 50 ), DP_CTRL_H, dpScreen );
	DP_Frame( x, inputY, w - DP_Wide( 50 ), DP_CTRL_H, ch.typing ? dpWhite : dpGlow );
	WG_Field( &style, x, inputY, w - DP_Wide( 50 ), DP_CTRL_H, ch.text, ch.typing, va( "A message to %s", ch.withName.c_str() ), DP_CursorX(), DP_CursorY() );
	click = DP_Click( &cx, &cy );
	if ( *click && cx >= x && cx < x + w - DP_Wide( 50 ) && cy >= inputY && cy < inputY + DP_CTRL_H ) {
		*click = qfalse;
		ch.typing = qtrue;
		WG_FieldClick( ch.text, cx, cy );
	}
	if ( DP_Button( x + w - DP_Wide( 46 ), inputY, DP_Wide( 46 ), chSendHttp.state == HTTP_BUSY ? "..." : "Send", qfalse ) )
		CH_Send();
}

static void CH_DrawSettings( float x, float y, float w ) {
	const float half = ( w - DP_Wide( 4 ) ) * 0.5f;

	DP_Text( x, y, w, "A message in the conversation open", dpText, DP_Font(), 0.6f, qfalse );
	y += 12;
	DP_Text( x, y, w, cl_chatSound->string[0] ? cl_chatSound->string : "none", dpDim, DP_Font(), 0.55f, qfalse );
	y += 12;
	if ( DP_Button( x, y, half * 0.66f, "Pick...", qfalse ) )
		CH_Pick( 1 );
	if ( DP_Button( x + half * 0.66f + DP_Wide( 4 ), y, half * 0.66f, "Hear it", qfalse ) )
		CH_Play( cl_chatSound->string );
	if ( DP_Button( x + ( half * 0.66f + DP_Wide( 4 ) ) * 2, y, half * 0.66f, "None", qfalse ) )
		Cvar_Set( "cl_chatSound", "" );
	y += DP_CTRL_H + 14;
	DP_Text( x, y, w, "Ringtone: a message while the conversation isn't open", dpText, DP_Font(), 0.6f, qfalse );
	y += 12;
	DP_Text( x, y, w, cl_chatRingtone->string[0] ? cl_chatRingtone->string : "none", dpDim, DP_Font(), 0.55f, qfalse );
	y += 12;
	if ( DP_Button( x, y, half * 0.66f, "Pick...", qfalse ) )
		CH_Pick( 2 );
	if ( DP_Button( x + half * 0.66f + DP_Wide( 4 ), y, half * 0.66f, "Hear it", qfalse ) )
		CH_Play( cl_chatRingtone->string );
	if ( DP_Button( x + ( half * 0.66f + DP_Wide( 4 ) ) * 2, y, half * 0.66f, "None", qfalse ) )
		Cvar_Set( "cl_chatRingtone", "" );
	y += DP_CTRL_H + 14;
	if ( DP_Button( x, y, w, cl_chatNotify->integer ? "Notices: on (a banner, as whispers have)" : "Notices: off", (qboolean)( cl_chatNotify->integer != 0 ) ) )
		Cvar_Set( "cl_chatNotify", cl_chatNotify->integer ? "0" : "1" );
	y += DP_CTRL_H + 14;
	if ( DP_Button( x, y, w, "Sounds as they were", qfalse ) ) {
		Cvar_Set( "cl_chatSound", CH_DEFAULT_SOUND );
		Cvar_Set( "cl_chatRingtone", CH_DEFAULT_RING );
	}
	y += DP_CTRL_H + 10;
	DP_Text( x, y, w, "Pick... opens the sound manager: Send there picks the sound.", dpDim, DP_Font(), 0.52f, qfalse );
}

void CL_Chat_Draw( const dpRect_t *s ) {
	const float pad = DP_Wide( 6 ), x = s->x + pad, w = s->w - pad * 2, bottom = s->y + s->h - 16;
	float y = s->y + DP_STATUS_H + 4;

	// the top row: back, whom, settings
	if ( ch.view != CH_LIST ) {
		if ( DP_Button( x, y, w * 0.3f, "< Chat", qfalse ) ) {
			ch.view = CH_LIST;
			ch.typing = qfalse;
			CH_AskContacts();
			return;
		}
		DP_Text( x + w * 0.32f, y + 2, w * 0.68f, ch.view == CH_TALK ? ch.withName.c_str() : "Settings", dpText, DP_Font(), 0.64f, qfalse );
	} else {
		DP_Text( x, y + 2, w * 0.6f, ch.totalUnread > 0 ? va( "Chat ^2(%i new)", ch.totalUnread ) : "Chat", dpText, DP_Font(), 0.66f, qfalse );
		if ( DP_Button( x + w * 0.7f, y, w * 0.3f, "Settings", qfalse ) ) {
			ch.view = CH_SETTINGS;
			return;
		}
	}
	y += DP_CTRL_H + 6;
	if ( ch.view == CH_LIST )
		CH_DrawList( x, y, w, bottom );
	else if ( ch.view == CH_TALK )
		CH_DrawTalk( x, y, w, bottom );
	else
		CH_DrawSettings( x, y, w );
	if ( ch.message[0] && cls.realtime - ch.messageTime < 5000 )
		DP_Text( x, s->y + s->h - 14, w, ch.message, dpWhite, DP_Font(), 0.55f, qfalse );
	WG_DrawOverlay();
}

/*
===============================================================================

Keys

===============================================================================
*/

qboolean CL_Chat_Key( int key ) {
	if ( key == A_MWHEELUP || key == A_MWHEELDOWN ) {
		const int dir = key == A_MWHEELUP ? -1 : 1;

		if ( ch.view == CH_TALK )
			ch.talkScroll = Q_max( 0.0f, ch.talkScroll - dir * 30.0f );
		else if ( ch.view == CH_LIST )
			ch.listScroll = Q_max( 0, ch.listScroll + dir );
		return qtrue;
	}
	if ( ch.view != CH_TALK || !ch.typing )
		return qfalse;
	if ( key == A_ENTER || key == A_KP_ENTER ) {
		CH_Send();
		return qtrue;
	}
	WG_EditKey( ch.text, sizeof( ch.text ), key, NULL );
	CH_Typed();
	// typing: none of the keys go on to the datapad's binds
	return qtrue;
}

void CL_Chat_Char( int ch_ ) {
	if ( ch.view == CH_TALK && ch.typing ) {
		WG_EditChar( ch.text, sizeof( ch.text ), ch_, NULL );
		CH_Typed();
	}
}

void CL_Chat_Disconnect( void ) {
	ch.contacts.clear();
	ch.messages.clear();
	ch.known = qfalse;
	ch.view = CH_LIST;
	ch.typing = qfalse;
	ch.totalUnread = 0;
}

void CL_Chat_Init( void ) {
	cl_chatNotify = Cvar_Get( "cl_chatNotify", "1", CVAR_ARCHIVE_ND, "The datapad's Chat: a notice and a ringtone for a message while its conversation isn't open" );
	cl_chatRingtone = Cvar_Get( "cl_chatRingtone", CH_DEFAULT_RING, CVAR_ARCHIVE_ND, "The datapad's Chat: the sound of a message come while its conversation isn't open" );
	cl_chatSound = Cvar_Get( "cl_chatSound", CH_DEFAULT_SOUND, CVAR_ARCHIVE_ND, "The datapad's Chat: the sound of a message in the conversation open, and of one sent" );
}
