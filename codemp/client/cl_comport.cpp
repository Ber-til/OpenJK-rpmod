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

// cl_comport.cpp -- the Comport on the datapad: JEDI's forum (phpBB), read and replied to
//
// The game talks to the forum itself, as a browser would: its pages are fetched and read for their
// parts (cl_html.cpp), and shown as the datapad's own pages: the forums, a forum's topics, a topic's
// posts. Logged in, a topic can be replied to.
//
// Logging in sends the name and password to the forum alone, over HTTPS: they're never kept, nor
// sent anywhere else. What the forum gives back is its cookies; with Stay logged in they're kept in
// datapad/comport.cookies in the home folder, so the next time starts logged in. Log out removes them.

#include <algorithm>
#include <map>
#include <string>
#include <vector>
#include "client.h"
#include "cl_datapad.h"
#include "cl_html.h"
#include "cl_http.h"
#include "cl_widgets.h"

#define CP_SITE			"https://comport.jediholo.net/"
#define CP_COOKIES		"datapad/comport.cookies"
#define CP_PAGE_BYTES	( 3 * 1024 * 1024 )
#define CP_NAME_LEN		64
#define CP_REPLY_LEN	4096
#define CP_WHEEL		36.0f
#define CP_PARAGRAPH	'\xB6'		// Enter in a reply: a new paragraph as it's sent

typedef enum {
	CP_LOGIN,
	CP_INDEX,
	CP_FORUM,
	CP_TOPIC,
	CP_PMS,			// the private messages: the inbox
	CP_PM			// one of them, read as a topic is, and answered
} cpPage_t;

#define CP_INBOX		CP_SITE "ucp.php?i=pm&folder=inbox"

typedef enum {
	CP_ACT_NONE,
	CP_ACT_LOGIN_FORM,		// the login page asked, for its hidden fields
	CP_ACT_LOGIN_WAIT,		// the login's form held a moment: phpBB refuses a form sent the second it was made
	CP_ACT_LOGIN,			// the name and password sent
	CP_ACT_REPLY_FORM,		// the reply page asked, for its fields
	CP_ACT_REPLY,			// the reply sent
	CP_ACT_LOGOUT
} cpAct_t;

typedef struct {
	bool		header;			// a category's name
	std::string	title, desc, url, counts;
} cpForum_t;

typedef struct {
	bool		pinned;			// sticky or announcement
	std::string	title, by, url, counts;
} cpTopic_t;

typedef struct {
	std::string					author, rank, date;
	std::vector<htmlBlock_t>	blocks;
} cpPost_t;

static struct {
	qboolean		shown, ready;
	std::map<std::string, std::string>	cookies;
	qboolean		stay;

	cpPage_t		page;
	cpPage_t		loadingPage;	// what the page asked will be
	std::string		url;			// the page on screen
	std::string		title, pageText, prevUrl, nextUrl, upUrl, replyUrl;
	std::vector<cpForum_t>	forums;
	std::vector<cpTopic_t>	topics;		// a forum's, or the inbox's messages
	int				newPms;			// unread private messages, as the forum's header says
	std::vector<cpPost_t>	posts;
	float			scroll, content, view;

	char			user[CP_NAME_LEN], pass[CP_NAME_LEN];
	char			retryPass[CP_NAME_LEN];	// kept for the one login tried again with ’ for ' (zeroed after)
	qboolean		curly;			// this login: the name's ' sent as ’
	char			reply[CP_REPLY_LEN];
	int				focus;			// 0 none, 1 the name, 2 the password, 3 the reply
	qboolean		replying;
	std::map<std::string, std::string>	form;	// a form's hidden fields, as the forum gave them
	std::string		formAction;
	std::string		formPage;		// the page the form was on, sent with it
	std::string		waitUrl, waitBody;	// the login, held until waitTime
	int				waitTime;
	int				formTime;		// when the reply's form came

	cpAct_t			act;
	char			message[256];
	int				messageTime;
} cp;

static httpRequest_t cpPageHttp, cpActHttp;

/*
===============================================================================

Cookies

===============================================================================
*/

static std::string CP_CookieHeader( void ) {
	std::string h;

	for ( const auto &c : cp.cookies )
		h += ( h.empty() ? "Cookie: " : "; " ) + c.first + "=" + c.second;
	return h.empty() ? h : h + "\r\n";
}

// a cookie's value by the end of its name (phpBB's are phpbb3_<something>_u, _k, _sid)
static std::string CP_Cookie( const char *suffix ) {
	const std::string end = suffix;

	for ( const auto &c : cp.cookies ) {
		if ( c.first.size() > end.size() && !c.first.compare( c.first.size() - end.size(), end.size(), end ) )
			return c.second;
	}
	return "";
}

static qboolean CP_LoggedIn( void ) {
	const std::string u = CP_Cookie( "_u" );

	return (qboolean)( !u.empty() && u != "1" );
}

static void CP_SaveCookies( void ) {
	std::string out = "// the Comport's cookies, kept with Stay logged in: they log this game in. Keep them to yourself\n";

	if ( !cp.stay || !CP_LoggedIn() ) {
		FS_HomeRemove( CP_COOKIES );
		return;
	}
	for ( const auto &c : cp.cookies )
		out += c.first + "=" + c.second + "\n";
	FS_WriteFile( CP_COOKIES, out.c_str(), (int)out.size() );
}

static void CP_LoadCookies( void ) {
	char *buf;
	const int len = FS_ReadFile( CP_COOKIES, (void **)&buf );

	if ( len <= 0 || !buf )
		return;
	for ( char *line = buf; line && *line; ) {
		char *next = strchr( line, '\n' ), *eq;

		if ( next )
			*next++ = '\0';
		eq = strchr( line, '=' );
		if ( eq && line[0] != '/' ) {
			*eq = '\0';
			cp.cookies[line] = eq + 1;
			cp.stay = qtrue;
		}
		line = next;
	}
	FS_FreeFile( buf );
}

// what an answer's Set-Cookie headers say: set, or gone
static void CP_TakeCookies( const char *headers ) {
	for ( const char *h = headers; h && *h; ) {
		const char *end = strstr( h, "\r\n" );
		const std::string line( h, end ? end - h : strlen( h ) );

		h = end ? end + 2 : NULL;
		if ( Q_stricmpn( line.c_str(), "set-cookie:", 11 ) )
			continue;
		{
			std::string v = line.substr( 11 );
			size_t eq, semi;
			std::string name, value;

			while ( !v.empty() && v[0] == ' ' )
				v.erase( 0, 1 );
			eq = v.find( '=' );
			semi = v.find( ';' );
			if ( eq == std::string::npos || ( semi != std::string::npos && semi < eq ) )
				continue;
			name = v.substr( 0, eq );
			value = v.substr( eq + 1, semi == std::string::npos ? std::string::npos : semi - eq - 1 );
			if ( value.empty() || value == "deleted" || Q_stristr( line.c_str(), "expires=Thu, 01-Jan-1970" ) || Q_stristr( line.c_str(), "max-age=0" ) )
				cp.cookies.erase( name );
			else
				cp.cookies[name] = value;
		}
	}
}

/*
===============================================================================

Asking the forum

===============================================================================
*/

static void CP_Message( const char *text ) {
	Q_strncpyz( cp.message, text, sizeof( cp.message ) );
	cp.messageTime = cls.realtime;
}

static qboolean CP_Request( httpRequest_t *req, const std::string &url, const char *form, qboolean noRedirect ) {
	char error[128];
	std::string headers = CP_CookieHeader();

	if ( req->state == HTTP_BUSY )
		return qfalse;
	// the forum's server turns away a form sent without the page it came from
	if ( form )
		headers += "Referer: " + ( cp.formPage.empty() ? std::string( CP_SITE "index.php" ) : cp.formPage ) + "\r\n";
	Q_strncpyz( req->headers, headers.c_str(), sizeof( req->headers ) );
	req->form = (qboolean)( form != NULL );
	req->noRedirect = noRedirect;
	req->maxAnswer = CP_PAGE_BYTES;
	if ( !HTTP_Start( req, url.c_str(), form, error, sizeof( error ) ) ) {
		CP_Message( va( "The Comport can't be reached: %s", error ) );
		return qfalse;
	}
	return qtrue;
}

static void CP_Load( cpPage_t page, const std::string &url ) {
	if ( CP_Request( &cpPageHttp, url, NULL, qfalse ) ) {
		cp.loadingPage = page;
		cp.url = url;
	}
}

// links on the forum's pages carry its session (sid=...): left out, the cookies say it
static std::string CP_Link( const std::string &href ) {
	const std::string url = HTML_Url( CP_SITE, href );
	const size_t q = url.find( '?' );
	std::string out, query;

	if ( q == std::string::npos )
		return url;
	out = url.substr( 0, q );
	query = url.substr( q + 1 );
	{
		const size_t hash = query.find( '#' );
		const std::string anchor = hash == std::string::npos ? "" : query.substr( hash );
		std::string kept;

		query = query.substr( 0, hash );
		for ( size_t a = 0; a <= query.size(); ) {
			size_t e = query.find( '&', a );
			const std::string part = query.substr( a, e == std::string::npos ? std::string::npos : e - a );

			if ( !part.empty() && part.compare( 0, 4, "sid=" ) )
				kept += ( kept.empty() ? "" : "&" ) + part;
			if ( e == std::string::npos )
				break;
			a = e + 1;
		}
		return out + ( kept.empty() ? "" : "?" + kept ) + anchor;
	}
}

// the element's text, or ""
static std::string CP_TextOf( const std::string &html, size_t from, size_t to, const char *tag, const char *cls ) {
	htmlElement_t el;

	if ( !HTML_Find( html, from, tag, cls, el ) || el.start >= to )
		return "";
	return HTML_Text( HTML_Inner( html, el ) );
}

// the first number in a text ("166 Posts" -> "166")
static std::string CP_Number( const std::string &text ) {
	const size_t a = text.find_first_of( "0123456789" );

	if ( a == std::string::npos )
		return "";
	return text.substr( a, text.find_first_not_of( "0123456789,", a ) - a );
}

// the previous and next pages' links, and "Page 1 of 3"
static void CP_ReadPages( const std::string &html ) {
	htmlElement_t el;
	size_t pos = 0;
	std::string text = CP_TextOf( html, 0, html.size(), "div", "pagination" );
	const size_t page = text.find( "Page " );

	cp.prevUrl.clear();
	cp.nextUrl.clear();
	cp.pageText = page != std::string::npos ? text.substr( page ) : "";
	while ( HTML_Find( html, pos, "a", "", el ) ) {
		const std::string rel = HTML_Attr( el.attrs, "rel" );

		pos = el.innerStart;
		if ( rel == "next" && cp.nextUrl.empty() )
			cp.nextUrl = CP_Link( HTML_Attr( el.attrs, "href" ) );
		else if ( rel == "prev" && cp.prevUrl.empty() )
			cp.prevUrl = CP_Link( HTML_Attr( el.attrs, "href" ) );
	}
}

static void CP_ReadIndex( const std::string &html ) {
	htmlElement_t li;
	size_t pos = 0;

	cp.forums.clear();
	cp.title = "Forums";
	while ( HTML_Find( html, pos, "li", "", li ) ) {
		const std::string cls = " " + HTML_Attr( li.attrs, "class" ) + " ";
		cpForum_t f;

		pos = li.innerStart;
		if ( cls.find( " header " ) != std::string::npos ) {
			f.header = true;
			f.title = CP_TextOf( html, li.innerStart, li.end, "a", "" );
			if ( f.title.empty() )
				f.title = CP_TextOf( html, li.innerStart, li.end, "dt", "" );
			if ( !f.title.empty() && f.title != "Topics" )
				cp.forums.push_back( f );
			pos = li.end;
		} else if ( cls.find( " row " ) != std::string::npos ) {
			htmlElement_t a, inner;

			if ( !HTML_Find( html, li.innerStart, "a", "forumtitle", a ) || a.start >= li.end ) {
				pos = li.end;
				continue;
			}
			f.header = false;
			f.title = HTML_Text( HTML_Inner( html, a ) );
			f.url = CP_Link( HTML_Attr( a.attrs, "href" ) );
			// the description: after the title, before what's hidden for small screens
			if ( HTML_Find( html, li.innerStart, "div", "list-inner", inner ) && inner.start < li.end ) {
				const size_t from = a.end, stop = html.find( "<div", from );

				if ( stop != std::string::npos && stop < inner.innerEnd )
					f.desc = HTML_Text( html.substr( from, stop - from ) );
			}
			{
				const std::string topics = CP_Number( CP_TextOf( html, li.innerStart, li.end, "dd", "topics" ) );
				const std::string posts = CP_Number( CP_TextOf( html, li.innerStart, li.end, "dd", "posts" ) );

				if ( !topics.empty() )
					f.counts = topics + " topics, " + posts + " posts";
			}
			cp.forums.push_back( f );
			pos = li.end;
		}
	}
}

static void CP_ReadForum( const std::string &html ) {
	htmlElement_t li;
	size_t pos = 0;

	cp.topics.clear();
	cp.title = CP_TextOf( html, 0, html.size(), "h2", "forum-title" );
	while ( HTML_Find( html, pos, "li", "row", li ) ) {
		htmlElement_t a;
		cpTopic_t t;
		const std::string cls = HTML_Attr( li.attrs, "class" );

		pos = li.end;
		if ( !HTML_Find( html, li.innerStart, "a", "topictitle", a ) || a.start >= li.end )
			continue;
		t.title = HTML_Text( HTML_Inner( html, a ) );
		t.url = CP_Link( HTML_Attr( a.attrs, "href" ) );
		t.pinned = (qboolean)( cls.find( "sticky" ) != std::string::npos || cls.find( "announce" ) != std::string::npos || cls.find( "global" ) != std::string::npos );
		t.by = CP_TextOf( html, li.innerStart, li.end, "div", "topic-poster" );
		{
			const std::string replies = CP_Number( CP_TextOf( html, li.innerStart, li.end, "dd", "posts" ) );
			const std::string views = CP_Number( CP_TextOf( html, li.innerStart, li.end, "dd", "views" ) );

			t.counts = replies + " replies, " + views + " views";
		}
		cp.topics.push_back( t );
	}
	CP_ReadPages( html );
	cp.upUrl = CP_SITE "index.php";
}

static void CP_ReadTopic( const std::string &html ) {
	htmlElement_t post, el;
	size_t pos = 0;

	cp.posts.clear();
	cp.title = CP_TextOf( html, 0, html.size(), "h2", "topic-title" );
	cp.replyUrl.clear();
	while ( HTML_Find( html, pos, "div", "post", post ) ) {
		cpPost_t p;

		pos = post.end;
		// who: the profile's name and rank; when: the author line's time
		if ( HTML_Find( html, post.innerStart, "dl", "postprofile", el ) && el.start < post.end ) {
			const std::string profile = HTML_Inner( html, el );
			htmlElement_t name;

			if ( HTML_Find( profile, 0, "a", "username-coloured", name ) || HTML_Find( profile, 0, "a", "username", name )
				|| HTML_Find( profile, 0, "span", "username", name ) )
				p.author = HTML_Text( HTML_Inner( profile, name ) );
			p.rank = CP_TextOf( profile, 0, profile.size(), "dd", "profile-rank" );
		}
		p.date = CP_TextOf( html, post.innerStart, post.end, "time", "" );
		if ( HTML_Find( html, post.innerStart, "div", "content", el ) && el.start < post.end )
			HTML_Blocks( HTML_Inner( html, el ), cp.url.c_str(), p.blocks );
		if ( !p.author.empty() || !p.blocks.empty() )
			cp.posts.push_back( p );
	}
	// a reply, if the forum offers one
	pos = 0;
	while ( HTML_Find( html, pos, "a", "", el ) ) {
		const std::string href = HTML_Attr( el.attrs, "href" );

		pos = el.innerStart;
		if ( href.find( "posting.php" ) != std::string::npos && href.find( "mode=reply" ) != std::string::npos ) {
			cp.replyUrl = CP_Link( href );
			break;
		}
	}
	CP_ReadPages( html );
	// back to its forum
	{
		const std::string f = HTML_Param( cp.url, "f" );

		cp.upUrl.clear();
		pos = 0;
		while ( HTML_Find( html, pos, "a", "", el ) ) {
			const std::string href = HTML_Attr( el.attrs, "href" );

			pos = el.innerStart;
			if ( href.find( "viewforum.php" ) != std::string::npos ) {
				cp.upUrl = CP_Link( href );		// the last in the breadcrumbs: this topic's forum
				if ( !f.empty() && HTML_Param( href, "f" ) == f )
					break;
			}
		}
	}
}

// the inbox: each message's subject, who sent it and when (prosilver's pm list, rows as a forum's)
static void CP_ReadPms( const std::string &html ) {
	htmlElement_t li;
	size_t pos = 0;

	cp.topics.clear();
	cp.title = "Private messages";
	while ( HTML_Find( html, pos, "li", "row", li ) ) {
		htmlElement_t a, dt;
		cpTopic_t t;
		const std::string cls = HTML_Attr( li.attrs, "class" );

		pos = li.end;
		if ( !HTML_Find( html, li.innerStart, "a", "topictitle", a ) || a.start >= li.end )
			continue;
		t.title = HTML_Text( HTML_Inner( html, a ) );
		t.url = CP_Link( HTML_Attr( a.attrs, "href" ) );
		t.pinned = (qboolean)( cls.find( "unread" ) != std::string::npos );
		// the line under the subject: by whom, when
		if ( HTML_Find( html, li.innerStart, "dt", "", dt ) && dt.start < li.end ) {
			std::string line = HTML_Text( HTML_Inner( html, dt ) );
			const size_t at = line.find( t.title );

			if ( at != std::string::npos )
				line.erase( at, t.title.size() );
			while ( !line.empty() && ( line[0] == ' ' || line[0] == '\n' ) )
				line.erase( 0, 1 );
			t.by = line;
		}
		cp.topics.push_back( t );
	}
	CP_ReadPages( html );
	cp.upUrl = CP_SITE "index.php";
}

// "Sent: <date> From: ..." or "Sent: <date> by ...": the date
static std::string CP_SentDate( const std::string &line ) {
	size_t from = line.find( "Sent:" ), to;

	if ( from == std::string::npos )
		return "";
	from += 5;
	to = line.find( "From:", from );
	if ( to == std::string::npos )
		to = line.find( " by ", from );
	std::string date = line.substr( from, to == std::string::npos ? std::string::npos : to - from );
	while ( !date.empty() && date[0] == ' ' )
		date.erase( 0, 1 );
	while ( !date.empty() && date[date.size() - 1] == ' ' )
		date.erase( date.size() - 1 );
	return date;
}

// one message (div.post.pm: its sender's profile, "Sent:" in p.author, div.content), then its history
// (div.topicreview's div.post, newest first: who and when in p.author), the one read left out of it;
// its subject, and the reply the forum offers
static void CP_ReadPm( const std::string &html ) {
	htmlElement_t post, el, review;
	size_t pos = 0, reviewStart = std::string::npos, reviewEnd = std::string::npos;
	const std::string current = HTML_Param( cp.url, "p" );

	cp.posts.clear();
	if ( HTML_Find( html, 0, "div", "topicreview", review ) ) {
		reviewStart = review.start;
		reviewEnd = review.end;
	}
	// the message
	while ( HTML_Find( html, pos, "div", "post", post ) && post.start < reviewStart ) {
		cpPost_t p;

		pos = post.end;
		if ( HTML_Find( html, post.innerStart, "dl", "postprofile", el ) && el.start < post.end ) {
			const std::string profile = HTML_Inner( html, el );
			htmlElement_t name;

			if ( HTML_Find( profile, 0, "a", "username-coloured", name ) || HTML_Find( profile, 0, "a", "username", name )
				|| HTML_Find( profile, 0, "span", "username", name ) )
				p.author = HTML_Text( HTML_Inner( profile, name ) );
			p.rank = CP_TextOf( profile, 0, profile.size(), "dd", "profile-rank" );
		}
		p.date = CP_SentDate( CP_TextOf( html, post.innerStart, post.end, "p", "author" ) );
		if ( HTML_Find( html, post.innerStart, "div", "content", el ) && el.start < post.end )
			HTML_Blocks( HTML_Inner( html, el ), cp.url.c_str(), p.blocks );
		if ( !p.author.empty() || !p.blocks.empty() ) {
			cp.posts.push_back( p );
			break;
		}
	}
	// the history, under a heading
	if ( reviewStart != std::string::npos ) {
		qboolean headed = qfalse;

		pos = review.innerStart;
		while ( HTML_Find( html, pos, "div", "post", post ) && post.start < reviewEnd ) {
			cpPost_t p;
			htmlElement_t body, name;
			const std::string author = CP_TextOf( html, post.innerStart, post.end, "p", "author" );

			pos = post.end;
			// the one read, again in its history
			if ( HTML_Find( html, post.innerStart, "div", "postbody", body ) && body.start < post.end
				&& !current.empty() && HTML_Attr( body.attrs, "id" ) == "pr" + current )
				continue;
			if ( HTML_Find( html, post.innerStart, "a", "username", name ) && name.start < post.end )
				p.author = HTML_Text( HTML_Inner( html, name ) );
			p.date = CP_SentDate( author );
			if ( HTML_Find( html, post.innerStart, "div", "content", el ) && el.start < post.end )
				HTML_Blocks( HTML_Inner( html, el ), cp.url.c_str(), p.blocks );
			if ( p.author.empty() && p.blocks.empty() )
				continue;
			if ( !headed ) {
				cpPost_t heading;

				heading.author = "Message history";
				heading.rank = "earlier messages, newest first";
				cp.posts.push_back( heading );
				headed = qtrue;
			}
			cp.posts.push_back( p );
		}
	}
	cp.title = CP_TextOf( html, 0, html.size(), "h3", "first" );
	if ( cp.title.empty() )
		cp.title = "Private message";
	cp.replyUrl.clear();
	pos = 0;
	while ( HTML_Find( html, pos, "a", "", el ) ) {
		const std::string href = HTML_Attr( el.attrs, "href" );

		pos = el.innerStart;
		if ( href.find( "ucp.php" ) != std::string::npos && href.find( "mode=compose" ) != std::string::npos
			&& href.find( "action=reply" ) != std::string::npos ) {
			cp.replyUrl = CP_Link( href );
			break;
		}
	}
	cp.prevUrl.clear();
	cp.nextUrl.clear();
	cp.pageText.clear();
	cp.upUrl = CP_INBOX;
}

// unread private messages, from the header's link to the inbox ("Private messages 2")
static void CP_ReadNewPms( const std::string &html ) {
	htmlElement_t el;
	size_t pos = 0;

	while ( HTML_Find( html, pos, "a", "", el ) ) {
		const std::string href = HTML_Attr( el.attrs, "href" );

		pos = el.innerStart;
		if ( href.find( "ucp.php" ) != std::string::npos && href.find( "i=pm" ) != std::string::npos
			&& href.find( "folder=inbox" ) != std::string::npos ) {
			const std::string text = HTML_Text( HTML_Inner( html, el ) );
			const size_t digit = text.find_first_of( "0123456789" );

			cp.newPms = digit == std::string::npos ? 0 : atoi( text.c_str() + digit );
			return;
		}
	}
}

static qboolean CP_IsLoginPage( const std::string &html ) {
	htmlElement_t form;

	return (qboolean)( HTML_Find( html, 0, "form", "", form ) && html.find( "id=\"login\"" ) != std::string::npos
		&& html.find( "name=\"password\"" ) != std::string::npos );
}

/*
===============================================================================

Logging in, replying, logging out

===============================================================================
*/

static std::string CP_FormBody( const std::map<std::string, std::string> &fields ) {
	std::string body;

	for ( const auto &f : fields )
		body += ( body.empty() ? "" : "&" ) + HTML_FormEncode( f.first ) + "=" + HTML_FormEncode( f.second );
	return body;
}

// an attribute that's there with no value, as checked or selected
static qboolean CP_HasAttr( const std::string &attrs, const char *name ) {
	const size_t len = strlen( name );

	for ( size_t at = 0; at + len <= attrs.size(); at++ ) {
		if ( Q_stricmpn( attrs.c_str() + at, name, (int)len ) )
			continue;
		if ( ( at == 0 || isspace( (unsigned char)attrs[at - 1] ) )
			&& ( at + len == attrs.size() || isspace( (unsigned char)attrs[at + len] ) || attrs[at + len] == '=' || attrs[at + len] == '/' ) )
			return qtrue;
	}
	return qfalse;
}

// what a browser sends of the form with this id: its text and hidden fields, the boxes ticked, the
// options picked, its text areas; no button (the one pressed is added by the caller), no file
static std::string CP_FormFields( const std::string &html, const char *id, std::map<std::string, std::string> &out ) {
	htmlElement_t form, el;
	size_t pos = 0;

	out.clear();
	for ( ;; ) {
		if ( !HTML_Find( html, pos, "form", "", form ) )
			return "";
		pos = form.innerStart;
		if ( HTML_Attr( form.attrs, "id" ) == id )
			break;
	}
	for ( pos = form.innerStart; HTML_Find( html, pos, "input", "", el ) && el.start < form.innerEnd; pos = el.end ) {
		const std::string name = HTML_Attr( el.attrs, "name" ), type = HTML_Attr( el.attrs, "type" );

		if ( name.empty() || out.count( name ) )
			continue;
		if ( !Q_stricmp( type.c_str(), "checkbox" ) || !Q_stricmp( type.c_str(), "radio" ) ) {
			if ( CP_HasAttr( el.attrs, "checked" ) ) {
				const std::string value = HTML_Attr( el.attrs, "value" );

				out[name] = value.empty() ? "on" : value;
			}
		} else if ( Q_stricmp( type.c_str(), "submit" ) && Q_stricmp( type.c_str(), "button" ) && Q_stricmp( type.c_str(), "image" )
			&& Q_stricmp( type.c_str(), "reset" ) && Q_stricmp( type.c_str(), "file" ) ) {
			out[name] = HTML_Attr( el.attrs, "value" );
		}
	}
	for ( pos = form.innerStart; HTML_Find( html, pos, "textarea", "", el ) && el.start < form.innerEnd; pos = el.end ) {
		const std::string name = HTML_Attr( el.attrs, "name" );

		if ( !name.empty() && !out.count( name ) )
			out[name] = HTML_Decode( HTML_Inner( html, el ) );
	}
	for ( pos = form.innerStart; HTML_Find( html, pos, "select", "", el ) && el.start < form.innerEnd; pos = el.end ) {
		const std::string name = HTML_Attr( el.attrs, "name" );
		htmlElement_t opt;
		std::string first, picked;
		qboolean any = qfalse;

		for ( size_t o = el.innerStart; HTML_Find( html, o, "option", "", opt ) && opt.start < el.innerEnd; o = opt.innerStart ) {
			const std::string value = CP_HasAttr( opt.attrs, "value" ) ? HTML_Attr( opt.attrs, "value" ) : HTML_Text( HTML_Inner( html, opt ) );

			if ( !any ) {
				first = value;
				any = qtrue;
			}
			if ( CP_HasAttr( opt.attrs, "selected" ) ) {
				picked = value;
				break;
			}
		}
		if ( !name.empty() && any && !out.count( name ) )
			out[name] = picked.empty() ? first : picked;
	}
	return HTML_Url( CP_SITE, HTML_Attr( form.attrs, "action" ) );
}

// what a page is: its first heading, or its title
static std::string CP_PageName( const std::string &html ) {
	std::string name = CP_TextOf( html, 0, html.size(), "h2", "" );

	if ( name.empty() )
		name = CP_TextOf( html, 0, html.size(), "title", "" );
	return name;
}

// a form's action, from the page
static std::string CP_FormAction( const std::string &html, const char *id ) {
	htmlElement_t form;
	size_t pos = 0;

	while ( HTML_Find( html, pos, "form", "", form ) ) {
		pos = form.innerStart;
		if ( !id[0] || HTML_Attr( form.attrs, "id" ) == id )
			return HTML_Url( CP_SITE, HTML_Attr( form.attrs, "action" ) );
	}
	return "";
}

// the name as a form field, its ' sent as ’ (U+2019) when curly: the game types no ’, and a name
// made on a phone often has one in place of ' (the forum takes them as different names)
static std::string CP_NameField( qboolean curly ) {
	std::string out;

	for ( const char *p = cp.user; *p; p++ ) {
		if ( curly && *p == '\'' )
			out += "%E2%80%99";
		else
			out += HTML_FormEncode( std::string( 1, *p ) );
	}
	return out;
}

static void CP_ForgetRetry( void ) {
	memset( cp.retryPass, 0, sizeof( cp.retryPass ) );
}

static void CP_StartLogin( void ) {
	if ( !cp.user[0] || !cp.pass[0] ) {
		CP_Message( "Type your Comport name and password" );
		return;
	}
	cp.curly = qfalse;
	CP_ForgetRetry();
	if ( CP_Request( &cpActHttp, CP_SITE "ucp.php?mode=login", NULL, qfalse ) ) {
		cp.act = CP_ACT_LOGIN_FORM;
		cp.formPage = CP_SITE "ucp.php?mode=login";
		CP_Message( "Logging in..." );
	}
}

static void CP_StartReply( void ) {
	if ( !cp.replyUrl.empty() && CP_Request( &cpActHttp, cp.replyUrl, NULL, qfalse ) ) {
		cp.act = CP_ACT_REPLY_FORM;
		cp.formPage = cp.replyUrl;
		cp.formAction.clear();
	}
}

static void CP_SendReply( void ) {
	std::map<std::string, std::string> fields = cp.form;
	std::string message = cp.reply;

	if ( cp.formAction.empty() || cls.realtime - cp.formTime < 1500 ) {
		CP_Message( "The reply form isn't ready yet: a moment" );
		return;
	}
	for ( char &c : message ) {
		if ( c == CP_PARAGRAPH )
			c = '\n';
	}
	if ( message.find_first_not_of( " \n" ) == std::string::npos ) {
		CP_Message( "Type your reply first" );
		return;
	}
	fields["message"] = message;
	fields["post"] = "Submit";
	if ( CP_Request( &cpActHttp, cp.formAction, CP_FormBody( fields ).c_str(), qtrue ) ) {
		cp.act = CP_ACT_REPLY;
		CP_Message( "Sending your reply..." );
	}
}

static void CP_Logout( void ) {
	const std::string sid = CP_Cookie( "_sid" );

	CP_Request( &cpActHttp, std::string( CP_SITE "ucp.php?mode=logout&sid=" ) + sid, NULL, qtrue );
	cp.act = CP_ACT_LOGOUT;
	cp.cookies.clear();
	cp.stay = qfalse;
	FS_HomeRemove( CP_COOKIES );
	cp.page = CP_LOGIN;
	CP_Message( "Logged out" );
}

// phpBB's own words for what went wrong, from its page
static std::string CP_ErrorText( const std::string &html ) {
	std::string e = CP_TextOf( html, 0, html.size(), "div", "error" );

	if ( e.empty() )
		e = CP_TextOf( html, 0, html.size(), "p", "error" );
	if ( e.empty() )
		e = CP_TextOf( html, 0, html.size(), "div", "message-content" );
	return e;
}

static void CP_ActAnswer( void ) {
	const std::string html = HTTP_Answer( &cpActHttp );

	CP_TakeCookies( cpActHttp.respHeaders );
	switch ( cp.act ) {
	case CP_ACT_LOGIN_FORM: {
		std::map<std::string, std::string> fields;
		const std::string action = CP_FormAction( html, "login" );

		HTML_Inputs( html, fields );
		if ( action.empty() || !fields.count( "form_token" ) ) {
			cp.act = CP_ACT_NONE;
			CP_ForgetRetry();
			CP_Message( "The Comport's login page didn't read as expected" );
			break;
		}
		// the password typed, or kept for the try with ’
		fields["password"] = cp.curly ? cp.retryPass : cp.pass;
		if ( !cp.curly && strchr( cp.user, '\'' ) )
			Q_strncpyz( cp.retryPass, cp.pass, sizeof( cp.retryPass ) );
		fields["login"] = "Login";
		if ( cp.stay )
			fields["autologin"] = "on";
		else
			fields.erase( "autologin" );
		fields.erase( "viewonline" );
		// sent in a moment (CL_Comport_Frame): the forum takes no form in the second it gave it
		cp.waitUrl = action;
		fields.erase( "username" );
		cp.waitBody = CP_FormBody( fields ) + "&username=" + CP_NameField( cp.curly );
		cp.waitTime = cls.realtime + 1500;
		cp.act = CP_ACT_LOGIN_WAIT;
		memset( cp.pass, 0, sizeof( cp.pass ) );
		break;
	}
	case CP_ACT_LOGIN:
		cp.act = CP_ACT_NONE;
		if ( CP_LoggedIn() ) {
			CP_ForgetRetry();
			CP_Message( va( "Logged in as %s", cp.user ) );
			CP_SaveCookies();
			CP_Load( CP_INDEX, CP_SITE "index.php" );
		} else {
			const std::string why = CP_ErrorText( html );

			// a name with ' the forum doesn't know: once more with ’, as a phone writes it
			if ( !cp.curly && cp.retryPass[0] && Q_stristr( why.c_str(), "username" )
				&& CP_Request( &cpActHttp, CP_SITE "ucp.php?mode=login", NULL, qfalse ) ) {
				cp.curly = qtrue;
				cp.act = CP_ACT_LOGIN_FORM;
				cp.formPage = CP_SITE "ucp.php?mode=login";
				CP_Message( "Logging in..." );
				break;
			}
			CP_ForgetRetry();
			CP_Message( why.empty() ? "Not logged in: check your name and password" : why.c_str() );
		}
		break;
	case CP_ACT_REPLY_FORM:
		cp.act = CP_ACT_NONE;
		cp.formAction = CP_FormFields( html, "postform", cp.form );
		cp.formTime = cls.realtime;
		if ( cp.formAction.empty() ) {
			const std::string why = CP_ErrorText( html );

			CP_Message( why.empty() ? "You can't reply to this topic" : why.c_str() );
			cp.replying = qfalse;
		}
		break;
	case CP_ACT_REPLY:
		cp.act = CP_ACT_NONE;
		// the forum sends the new post's page: there; or says it's posted, on a page that goes on to it
		if ( cpActHttp.status == 302 || cpActHttp.status == 301 || cpActHttp.status == 303
			|| ( cpActHttp.status == 200 && ( Q_stristr( html.c_str(), "posted successfully" ) || Q_stristr( html.c_str(), "sent successfully" )
				|| Q_stristr( html.c_str(), "http-equiv=\"refresh\"" ) ) ) ) {
			const char *loc = Q_stristr( cpActHttp.respHeaders, "location:" );
			std::string where = cp.url;

			if ( loc ) {
				const char *end = strstr( loc, "\r\n" );

				where = std::string( loc + 9, end ? end - loc - 9 : strlen( loc + 9 ) );
				while ( !where.empty() && where[0] == ' ' )
					where.erase( 0, 1 );
				where = CP_Link( where );
			}
			cp.reply[0] = '\0';
			cp.replying = qfalse;
			cp.focus = 0;
			if ( cp.page == CP_PM ) {
				// a private message answered: back to the inbox
				CP_Message( "Your answer is sent" );
				CP_Load( CP_PMS, CP_INBOX );
				break;
			}
			CP_Message( "Your reply is posted" );
			CP_Load( CP_TOPIC, where );
		} else {
			const std::string why = CP_ErrorText( html );

			// what the forum answered, to tell what went wrong
			Com_Printf( "Comport: the reply got %i, \"%s\"%s%s\n", cpActHttp.status, CP_PageName( html ).c_str(),
				why.empty() ? "" : ": ", why.c_str() );
			CP_Message( why.empty() ? va( "The reply didn't go through: the forum answered \"%s\" (%i)", CP_PageName( html ).c_str(),
				cpActHttp.status ) : why.c_str() );
			// a fresh form for the next try
			CP_StartReply();
		}
		break;
	default:
		cp.act = CP_ACT_NONE;
		break;
	}
	HTTP_Release( &cpActHttp );
}

void CL_Comport_Frame( void ) {
	if ( HTTP_Poll( &cpPageHttp ) ) {
		const std::string html = HTTP_Answer( &cpPageHttp );

		CP_TakeCookies( cpPageHttp.respHeaders );
		if ( cpPageHttp.status != 200 ) {
			CP_Message( va( "The Comport answered %i", cpPageHttp.status ) );
		} else if ( cp.loadingPage != CP_INDEX && CP_IsLoginPage( html ) ) {
			cp.page = CP_LOGIN;
			CP_Message( "Log in to read that" );
		} else {
			cp.page = cp.loadingPage;
			cp.scroll = 0;
			if ( cp.page == CP_INDEX )
				CP_ReadIndex( html );
			else if ( cp.page == CP_FORUM )
				CP_ReadForum( html );
			else if ( cp.page == CP_TOPIC )
				CP_ReadTopic( html );
			else if ( cp.page == CP_PMS )
				CP_ReadPms( html );
			else if ( cp.page == CP_PM )
				CP_ReadPm( html );
			if ( CP_LoggedIn() )
				CP_ReadNewPms( html );
			// the forum may have given a new session: kept with the rest
			if ( cp.stay )
				CP_SaveCookies();
		}
		HTTP_Release( &cpPageHttp );
	}
	if ( HTTP_Poll( &cpActHttp ) )
		CP_ActAnswer();
	if ( cp.act == CP_ACT_LOGIN_WAIT && cls.realtime >= cp.waitTime ) {
		cp.act = CP_Request( &cpActHttp, cp.waitUrl, cp.waitBody.c_str(), qtrue ) ? CP_ACT_LOGIN : CP_ACT_NONE;
		// the password's gone from the game as soon as it's sent
		std::fill( cp.waitBody.begin(), cp.waitBody.end(), '\0' );
		cp.waitBody.clear();
	}
}

void CL_Comport_Shown( qboolean shown ) {
	// the first time: the forums if the cookies kept say we're logged in, else logging in
	if ( shown && !cp.ready ) {
		cp.ready = qtrue;
		cp.page = CP_LOGIN;
		if ( CP_LoggedIn() )
			CP_Load( CP_INDEX, CP_SITE "index.php" );
	}
	cp.shown = shown;
	cp.focus = 0;
}

/*
===============================================================================

Pages

===============================================================================
*/

static void CP_FieldStyle( wgStyle_t *style ) {
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

static void CP_Field( float x, float y, float w, char *buf, int which, const char *placeholder, qboolean secret ) {
	wgStyle_t style;
	char shown[CP_NAME_LEN];

	CP_FieldStyle( &style );
	DP_Fill( x, y, w, DP_CTRL_H, dpButton );
	DP_Frame( x, y, w, DP_CTRL_H, cp.focus == which ? dpWhite : dpGlow );
	if ( secret ) {
		// as many * as it has letters
		const int len = (int)strlen( buf );

		memset( shown, '*', len );
		shown[len] = '\0';
		WG_Field( &style, x, y, w, DP_CTRL_H, shown, (qboolean)( cp.focus == which ), placeholder, DP_CursorX(), DP_CursorY() );
	} else {
		WG_Field( &style, x, y, w, DP_CTRL_H, buf, (qboolean)( cp.focus == which ), placeholder, DP_CursorX(), DP_CursorY() );
	}
	if ( DP_Clicked( x, y, w, DP_CTRL_H ) ) {
		if ( cp.focus != which )
			WG_EditReset();
		cp.focus = which;
		if ( !secret )
			WG_FieldClick( buf, DP_CursorX(), DP_CursorY() );
	}
}

// the bar at the top: back, the page's name, who's logged in
static float CP_TopBar( const dpRect_t *s, const char *title, cpPage_t back, const std::string &backUrl ) {
	const float x = s->x + DP_Wide( 8 ), w = s->w - DP_Wide( 16 );
	float y = s->y + DP_STATUS_H + 4, tx = x;

	if ( back != CP_LOGIN ) {
		if ( DP_Button( x, y, DP_Wide( 46 ), "< Back", qfalse ) )
			CP_Load( back, backUrl );
		tx += DP_Wide( 52 );
	}
	DP_Text( tx, y + 1, w - ( tx - x ) - DP_Wide( CP_LoggedIn() ? 128 : 70 ), title, dpText, DP_Font(), 0.66f, qfalse );
	if ( CP_LoggedIn() ) {
		// the private messages, how many unread on it
		if ( DP_Button( x + w - DP_Wide( 64 + 4 + 54 ), y, DP_Wide( 54 ), cp.newPms > 0 ? va( "PMs (%i)", cp.newPms ) : "PMs",
			(qboolean)( cp.page == CP_PMS || cp.page == CP_PM ) ) )
			CP_Load( CP_PMS, CP_INBOX );
		if ( DP_Button( x + w - DP_Wide( 64 ), y, DP_Wide( 64 ), "Log out", qfalse ) )
			CP_Logout();
	} else if ( DP_Button( x + w - DP_Wide( 64 ), y, DP_Wide( 64 ), "Log in", qfalse ) ) {
		cp.page = CP_LOGIN;
	}
	return y + DP_CTRL_H + 6;
}

static void CP_DrawLogin( const dpRect_t *s ) {
	const float x = s->x + DP_Wide( 24 ), w = s->w - DP_Wide( 48 );
	float y = s->y + DP_STATUS_H + 20;

	DP_Text( s->x, y, s->w, "COMPORT", dpText, DP_TitleFont(), 0.9f, qtrue );
	y += 26;
	DP_Text( s->x, y, s->w, "JEDI's forum. Log in with your Comport account.", dpDim, DP_Font(), 0.6f, qtrue );
	y += 26;
	DP_Text( x, y, w, "Name", dpDim, DP_Font(), 0.6f, qfalse );
	y += 12;
	CP_Field( x, y, w, cp.user, 1, "your Comport name", qfalse );
	y += DP_CTRL_H + 8;
	DP_Text( x, y, w, "Password", dpDim, DP_Font(), 0.6f, qfalse );
	y += 12;
	CP_Field( x, y, w, cp.pass, 2, "your password", qtrue );
	y += DP_CTRL_H + 8;
	if ( DP_Button( x, y, w * 0.5f - DP_Wide( 2 ), cp.stay ? "Stay logged in: on" : "Stay logged in: off", cp.stay ) )
		cp.stay = (qboolean)!cp.stay;
	if ( DP_Button( x + w * 0.5f + DP_Wide( 2 ), y, w * 0.5f - DP_Wide( 2 ), cp.act == CP_ACT_LOGIN_FORM || cp.act == CP_ACT_LOGIN_WAIT || cp.act == CP_ACT_LOGIN ? "Logging in..." : "Log in", qfalse ) )
		CP_StartLogin();
	y += DP_CTRL_H + 14;
	if ( DP_Button( x, y, w, "Read as a guest", qfalse ) )
		CP_Load( CP_INDEX, CP_SITE "index.php" );
	y += DP_CTRL_H + 18;
	DP_Text( x, y, w, "Your password goes to the Comport alone, over HTTPS,", dpDim, DP_Font(), 0.55f, qfalse );
	DP_Text( x, y + 11, w, "and is never kept. Stay logged in keeps only the forum's", dpDim, DP_Font(), 0.55f, qfalse );
	DP_Text( x, y + 22, w, "cookies on this computer.", dpDim, DP_Font(), 0.55f, qfalse );
}

// a row clickable while it's on the screen
static qboolean CP_Row( float x, float y, float w, float h, float top, float bottom ) {
	const qboolean shown = (qboolean)( y >= top && y + h <= bottom );
	const qboolean hover = (qboolean)( shown && DP_InRect( x, y, w, h ) );

	if ( shown )
		DP_Fill( x, y, w, h, hover ? dpButtonHover : dpTile );
	return (qboolean)( shown && DP_Clicked( x, y, w, h ) );
}

static void CP_DrawIndex( const dpRect_t *s ) {
	const float x = s->x + DP_Wide( 8 ), w = s->w - DP_Wide( 18 ), top = CP_TopBar( s, "Forums", CP_LOGIN, "" ), bottom = s->y + s->h - 4;
	float y = top - cp.scroll;

	for ( const cpForum_t &f : cp.forums ) {
		if ( f.header ) {
			if ( y >= top && y + 12 <= bottom )
				DP_Text( x, y + 2, w, f.title.c_str(), dpDim, DP_Font(), 0.6f, qfalse );
			y += 16;
			continue;
		}
		if ( CP_Row( x, y, w, 38, top, bottom ) )
			CP_Load( CP_FORUM, f.url );
		if ( y >= top && y + 38 <= bottom ) {
			DP_Text( x + DP_Wide( 4 ), y + 2, w - DP_Wide( 8 ), f.title.c_str(), dpText, DP_Font(), 0.62f, qfalse );
			DP_Text( x + DP_Wide( 4 ), y + 14, w - DP_Wide( 8 ), f.desc.c_str(), dpDim, DP_Font(), 0.55f, qfalse );
			DP_Text( x + DP_Wide( 4 ), y + 25, w - DP_Wide( 8 ), f.counts.c_str(), dpDim, DP_Font(), 0.52f, qfalse );
		}
		y += 41;
	}
	if ( cp.forums.empty() )
		DP_Text( x, top + 6, w, cpPageHttp.state == HTTP_BUSY ? "Reading the Comport..." : "No forums to show", dpDim, DP_Font(), 0.6f, qfalse );
	cp.view = bottom - top;
	cp.content = y + cp.scroll - top;
	DP_ScrollBar( s->x + s->w - DP_Wide( 6 ), top, cp.view, cp.scroll, cp.content );
}

// < and > for a forum's or a topic's pages
static void CP_Pages( float x, float y, float w, cpPage_t page ) {
	if ( DP_Button( x, y, DP_Wide( 24 ), "<", qfalse ) && !cp.prevUrl.empty() )
		CP_Load( page, cp.prevUrl );
	DP_Text( x + DP_Wide( 28 ), y + 2, w - DP_Wide( 56 ), cp.pageText.c_str(), dpDim, DP_Font(), 0.58f, qtrue );
	if ( DP_Button( x + w - DP_Wide( 24 ), y, DP_Wide( 24 ), ">", qfalse ) && !cp.nextUrl.empty() )
		CP_Load( page, cp.nextUrl );
}

// a forum's topics, or the inbox's messages (pinned: unread)
static void CP_DrawForum( const dpRect_t *s ) {
	const qboolean pms = (qboolean)( cp.page == CP_PMS );
	const float x = s->x + DP_Wide( 8 ), w = s->w - DP_Wide( 18 );
	const float top = CP_TopBar( s, cp.title.c_str(), CP_INDEX, CP_SITE "index.php" ) + DP_CTRL_H + 6, bottom = s->y + s->h - 4;
	float y = top - cp.scroll;

	CP_Pages( x, top - DP_CTRL_H - 6, w, pms ? CP_PMS : CP_FORUM );
	for ( const cpTopic_t &t : cp.topics ) {
		if ( CP_Row( x, y, w, 38, top, bottom ) )
			CP_Load( pms ? CP_PM : CP_TOPIC, t.url );
		if ( y >= top && y + 38 <= bottom ) {
			DP_Text( x + DP_Wide( 4 ), y + 2, w - DP_Wide( 8 ), va( "%s%s", t.pinned ? ( pms ? "^2new ^7" : "^3* ^7" ) : "", t.title.c_str() ), dpText, DP_Font(), 0.62f, qfalse );
			DP_Text( x + DP_Wide( 4 ), y + 14, w - DP_Wide( 8 ), t.by.c_str(), dpDim, DP_Font(), 0.55f, qfalse );
			DP_Text( x + DP_Wide( 4 ), y + 25, w - DP_Wide( 8 ), t.counts.c_str(), dpDim, DP_Font(), 0.52f, qfalse );
		}
		y += 41;
	}
	if ( cp.topics.empty() )
		DP_Text( x, top + 6, w, cpPageHttp.state == HTTP_BUSY ? "Reading..." : pms ? "No private messages" : "No topics you can see here", dpDim, DP_Font(), 0.6f, qfalse );
	cp.view = bottom - top;
	cp.content = y + cp.scroll - top;
	DP_ScrollBar( s->x + s->w - DP_Wide( 6 ), top, cp.view, cp.scroll, cp.content );
}

static void CP_DrawTopic( const dpRect_t *s ) {
	const float x = s->x + DP_Wide( 8 ), w = s->w - DP_Wide( 18 );
	const qboolean pm = (qboolean)( cp.page == CP_PM );
	const float top = CP_TopBar( s, cp.title.c_str(), pm ? CP_PMS : CP_FORUM, cp.upUrl.empty() ? std::string( CP_SITE "index.php" ) : cp.upUrl ) + DP_CTRL_H + 6;
	const float bottom = s->y + s->h - 4;
	float y = top - cp.scroll;

	if ( !pm )
		CP_Pages( x, top - DP_CTRL_H - 6, w - DP_Wide( 70 ), CP_TOPIC );
	if ( DP_Button( x + w - DP_Wide( 64 ), top - DP_CTRL_H - 6, DP_Wide( 64 ), "On the web", qfalse ) )
		Sys_OpenPath( cp.url.c_str() );
	for ( const cpPost_t &p : cp.posts ) {
		// who and when, then what they wrote
		if ( y + 22 >= top && y <= bottom ) {
			const float y0 = Q_max( y, top ), y1 = Q_min( y + 22, bottom );

			if ( y1 > y0 )
				DP_Fill( x, y0, w, y1 - y0, dpTile );
			if ( y >= top && y + 22 <= bottom ) {
				DP_Text( x + DP_Wide( 4 ), y + 2, w * 0.6f, p.author.c_str(), dpWhite, DP_Font(), 0.62f, qfalse );
				DP_Text( x + DP_Wide( 4 ), y + 12, w * 0.6f, p.rank.c_str(), dpDim, DP_Font(), 0.52f, qfalse );
				DP_Text( x + w * 0.55f, y + 7, w * 0.45f - DP_Wide( 4 ), p.date.c_str(), dpDim, DP_Font(), 0.52f, qfalse );
			}
		}
		y += 26;
		y += DP_DrawBlocks( p.blocks, x + DP_Wide( 4 ), y, w - DP_Wide( 8 ), top, bottom );
		if ( y >= top && y <= bottom )
			DP_Fill( x, y, w, 1, dpGlow );
		y += 8;
	}

	// a reply, at the bottom
	if ( CP_LoggedIn() && !cp.replyUrl.empty() ) {
		if ( !cp.replying ) {
			if ( y >= top && y + DP_CTRL_H <= bottom && DP_Button( x, y, w, pm ? "Answer" : "Reply", qfalse ) ) {
				cp.replying = qtrue;
				cp.focus = 3;
				CP_StartReply();
			}
			y += DP_CTRL_H + 6;
		} else {
			const float boxH = 90;

			if ( y >= top && y + boxH + DP_CTRL_H + 4 <= bottom ) {
				wgStyle_t style;

				CP_FieldStyle( &style );
				DP_Fill( x, y, w, boxH, dpButton );
				DP_Frame( x, y, w, boxH, cp.focus == 3 ? dpWhite : dpGlow );
				WG_TextBox( &style, x, y, w, boxH, cp.reply, (qboolean)( cp.focus == 3 ), pm ? "Your answer (Enter: a new paragraph; BBCode as on the site)"
					: "Your reply (Enter: a new paragraph; BBCode as on the site)", DP_CursorX(), DP_CursorY() );
				if ( DP_Clicked( x, y, w, boxH ) ) {
					cp.focus = 3;
					WG_FieldClick( cp.reply, DP_CursorX(), DP_CursorY() );
				}
				if ( DP_Button( x, y + boxH + 4, w * 0.5f - DP_Wide( 2 ), cp.act == CP_ACT_REPLY ? "Sending..." : "Send", qfalse ) )
					CP_SendReply();
				if ( DP_Button( x + w * 0.5f + DP_Wide( 2 ), y + boxH + 4, w * 0.5f - DP_Wide( 2 ), "Cancel", qfalse ) ) {
					cp.replying = qfalse;
					cp.focus = 0;
				}
			}
			y += boxH + DP_CTRL_H + 10;
		}
	} else if ( !CP_LoggedIn() ) {
		if ( y >= top && y + 12 <= bottom )
			DP_Text( x, y, w, "Log in to reply", dpDim, DP_Font(), 0.58f, qfalse );
		y += 16;
	}
	if ( cp.posts.empty() )
		DP_Text( x, top + 6, w, cpPageHttp.state == HTTP_BUSY ? "Reading..." : "No posts to show", dpDim, DP_Font(), 0.6f, qfalse );
	cp.view = bottom - top;
	cp.content = y + cp.scroll - top;
	DP_ScrollBar( s->x + s->w - DP_Wide( 6 ), top, cp.view, cp.scroll, cp.content );
}

void CL_Comport_Draw( const dpRect_t *s ) {
	switch ( cp.page ) {
	case CP_INDEX:	CP_DrawIndex( s );	break;
	case CP_FORUM:	CP_DrawForum( s );	break;
	case CP_TOPIC:	CP_DrawTopic( s );	break;
	case CP_PMS:	CP_DrawForum( s );	break;
	case CP_PM:		CP_DrawTopic( s );	break;
	default:		CP_DrawLogin( s );	break;
	}
	if ( cp.message[0] && cls.realtime - cp.messageTime < 5000 ) {
		DP_Fill( s->x + 2, s->y + s->h - 16, s->w - 4, 14, dpScreen );
		DP_Text( s->x + DP_Wide( 8 ), s->y + s->h - 15, s->w - DP_Wide( 16 ), cp.message, dpWhite, DP_Font(), 0.58f, qfalse );
	}
	WG_DrawOverlay();
}

/*
===============================================================================

Keys

===============================================================================
*/

static char *CP_FocusedBuf( int *size ) {
	*size = cp.focus == 3 ? CP_REPLY_LEN : CP_NAME_LEN;
	return cp.focus == 1 ? cp.user : cp.focus == 2 ? cp.pass : cp.focus == 3 ? cp.reply : NULL;
}

qboolean CL_Comport_Key( int key ) {
	int size;
	char *buf = CP_FocusedBuf( &size );

	if ( buf ) {
		if ( key == A_ENTER || key == A_KP_ENTER ) {
			if ( cp.focus == 3 )
				WG_EditChar( buf, size, (unsigned char)CP_PARAGRAPH, NULL );
			else if ( cp.focus == 1 )
				cp.focus = 2;
			else
				CP_StartLogin();
			return qtrue;
		}
		if ( key == A_TAB ) {
			cp.focus = cp.focus == 1 ? 2 : cp.focus == 2 ? 1 : cp.focus;
			return qtrue;
		}
		if ( key == A_MWHEELUP || key == A_MWHEELDOWN ) {
			// the wheel still scrolls the page
		} else {
			WG_EditKey( buf, size, key, NULL );
			return qtrue;
		}
	}
	{
		const float page = cp.view > 0 ? cp.view - 30 : 200;
		float step = 0;

		if ( key == A_MWHEELUP || key == A_CURSOR_UP )
			step = -CP_WHEEL;
		else if ( key == A_MWHEELDOWN || key == A_CURSOR_DOWN )
			step = CP_WHEEL;
		else if ( key == A_PAGE_UP )
			step = -page;
		else if ( key == A_PAGE_DOWN )
			step = page;
		else if ( key == A_HOME )
			step = -1e6f;
		else if ( key == A_END )
			step = 1e6f;
		else
			return qfalse;
		cp.scroll = Com_Clamp( 0.0f, Q_max( 0.0f, cp.content - cp.view ), cp.scroll + step );
		return qtrue;
	}
}

void CL_Comport_Char( int ch ) {
	int size;
	char *buf = CP_FocusedBuf( &size );

	if ( buf )
		WG_EditChar( buf, size, ch, NULL );
}

// datapad comport guest | <forum number>: the forums, or one of them
void CL_Comport_Open( const char *what ) {
	if ( !Q_stricmp( what, "guest" ) )
		CP_Load( CP_INDEX, CP_SITE "index.php" );
	else if ( atoi( what ) > 0 )
		CP_Load( CP_FORUM, va( CP_SITE "viewforum.php?f=%i", atoi( what ) ) );
}

void CL_Comport_Init( void ) {
	CP_LoadCookies();
}
