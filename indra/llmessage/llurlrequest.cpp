/** 
 * @file llurlrequest.cpp
 * @author Phoenix
 * @date 2005-04-28
 * @brief Implementation of the URLRequest class and related classes.
 *
 * $LicenseInfo:firstyear=2005&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 * 
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * 
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "linden_common.h"
#include "llurlrequest.h"

#ifdef CWDEBUG
#include <libcwd/buf2str.h>
#endif

#include <algorithm>
#include <openssl/x509_vfy.h>
#include <openssl/ssl.h>
#include "aicurleasyrequeststatemachine.h"
#include "llioutil.h"
#include "llmemtype.h"
#include "llpumpio.h"
#include "llsd.h"
#include "llstring.h"
#include "apr_env.h"
#include "llapr.h"
#include "llscopedvolatileaprpool.h"
#include "llfasttimer.h"
#include "message.h"
static const U32 HTTP_STATUS_PIPE_ERROR = 499;

/**
 * String constants
 */
const std::string CONTEXT_TRANSFERED_BYTES("transfered_bytes");

/**
 * class LLURLRequest
 */

// static
std::string LLURLRequest::actionAsVerb(LLURLRequest::ERequestAction action)
{
	static int const array_size = HTTP_MOVE + 1;	// INVALID == 0
	static char const* const VERBS[array_size] =
	{
		"(invalid)",
		"HEAD",
		"GET",
		"PUT",
		"POST",
		"DELETE",
		"MOVE"
	};
	return VERBS[action >= array_size ? INVALID : action];
}

// This might throw AICurlNoEasyHandle.
LLURLRequest::LLURLRequest(LLURLRequest::ERequestAction action, std::string const& url, Injector* body,
	LLHTTPClient::ResponderPtr responder, AIHTTPHeaders& headers, bool is_auth, bool no_compression) :
    mAction(action), mURL(url), mIsAuth(is_auth), mNoCompression(no_compression),
	mBody(body), mResponder(responder), mHeaders(headers)
{
}

void LLURLRequest::initialize_impl(void)
{
	if (mHeaders.hasHeader("Cookie"))
	{
		allowCookies();
	}

	// If the header is "Pragma" with no value, the caller intends to
	// force libcurl to drop the Pragma header it so gratuitously inserts.
	// Before inserting the header, force libcurl to not use the proxy.
	std::string pragma_value;
	if (mHeaders.getValue("Pragma", pragma_value) && pragma_value.empty())
	{
		useProxy(false);
	}

	if (mAction == HTTP_PUT || mAction == HTTP_POST)
	{
		// If the Content-Type header was passed in we defer to the caller's wisdom,
		// but if they did not specify a Content-Type, then ask the injector.
		mHeaders.addHeader("Content-Type", mBody->contentType(), AIHTTPHeaders::keep_existing_header);
	}
	else
	{
		// Check to see if we have already set Accept or not. If no one
		// set it, set it to application/llsd+xml since that's what we
		// almost always want.
		mHeaders.addHeader("Accept", "application/llsd+xml", AIHTTPHeaders::keep_existing_header);
	}

	if (mAction == HTTP_POST && gMessageSystem)
	{
		mHeaders.addHeader("X-SecondLife-UDP-Listen-Port", llformat("%d", gMessageSystem->mPort));
	}

	bool success = false;
	try
	{
		AICurlEasyRequest_wat easy_request_w(*mCurlEasyRequest);
		easy_request_w->prepRequest(easy_request_w, mHeaders, mResponder);

		if (mBody)
		{
			// This might throw AICurlNoBody.
			mBodySize = mBody->get_body(easy_request_w->sChannels, easy_request_w->getInput());
		}

		success = configure(easy_request_w);
	}
	catch (AICurlNoBody const& error)
	{
		llwarns << "Injector::get_body() failed: " << error.what() << llendl; 
	}

	if (success)
	{
		// Continue to initialize base class.
		AICurlEasyRequestStateMachine::initialize_impl();
	}
	else
	{
		abort();
	}
}

void LLURLRequest::addHeader(const char* header)
{
	AICurlEasyRequest_wat curlEasyRequest_w(*mCurlEasyRequest);
	curlEasyRequest_w->addHeader(header);
}

// Added to mitigate the effect of libcurl looking
// for the ALL_PROXY and http_proxy env variables
// and deciding to insert a Pragma: no-cache
// header! The only usage of this method at the
// time of this writing is in llhttpclient.cpp
// in the request() method, where this method
// is called with use_proxy = FALSE
void LLURLRequest::useProxy(bool use_proxy)
{
    static std::string env_proxy;

    if (use_proxy && env_proxy.empty())
    {
		char* env_proxy_str;
        LLScopedVolatileAPRPool scoped_pool;
        apr_status_t status = apr_env_get(&env_proxy_str, "ALL_PROXY", scoped_pool);
        if (status != APR_SUCCESS)
        {
			status = apr_env_get(&env_proxy_str, "http_proxy", scoped_pool);
        }
        if (status != APR_SUCCESS)
        {
            use_proxy = false;
        }
		else
		{
			// env_proxy_str is stored in the scoped_pool, so we have to make a copy.
			env_proxy = env_proxy_str;
		}
    }

	LL_DEBUGS("Proxy") << "use_proxy = " << (use_proxy?'Y':'N') << ", env_proxy = " << (!env_proxy.empty() ? env_proxy : "(null)") << LL_ENDL;

	AICurlEasyRequest_wat curlEasyRequest_w(*mCurlEasyRequest);
	curlEasyRequest_w->setoptString(CURLOPT_PROXY, (use_proxy && !env_proxy.empty()) ? env_proxy : std::string(""));
}

#ifdef AI_UNUSED
void LLURLRequest::useProxy(const std::string &proxy)
{
	AICurlEasyRequest_wat curlEasyRequest_w(*mCurlEasyRequest);
    curlEasyRequest_w->setoptString(CURLOPT_PROXY, proxy);
}
#endif

void LLURLRequest::allowCookies()
{
	AICurlEasyRequest_wat curlEasyRequest_w(*mCurlEasyRequest);
	curlEasyRequest_w->setoptString(CURLOPT_COOKIEFILE, "");
}

bool LLURLRequest::configure(AICurlEasyRequest_wat const& curlEasyRequest_w)
{
	bool rv = false;
	{
		switch(mAction)
		{
		case HTTP_HEAD:
			curlEasyRequest_w->setopt(CURLOPT_HEADER, 1);
			curlEasyRequest_w->setopt(CURLOPT_NOBODY, 1);
			curlEasyRequest_w->setopt(CURLOPT_FOLLOWLOCATION, 1);
			rv = true;
			break;

		case HTTP_GET:
			curlEasyRequest_w->setopt(CURLOPT_HTTPGET, 1);
			curlEasyRequest_w->setopt(CURLOPT_FOLLOWLOCATION, 1);

			// Set Accept-Encoding to allow response compression
			curlEasyRequest_w->setoptString(CURLOPT_ENCODING, mNoCompression ? "identity" : "");
			rv = true;
			break;

		case HTTP_PUT:
		{
			// Disable the expect http 1.1 extension. POST and PUT default
			// to using this, causing the broken server to get confused.
			curlEasyRequest_w->addHeader("Expect:");
			curlEasyRequest_w->setopt(CURLOPT_UPLOAD, 1);
			curlEasyRequest_w->setopt(CURLOPT_INFILESIZE, mBodySize);
			rv = true;
			break;
		}
		case HTTP_POST:
		{
			// Set the handle for an http post
			curlEasyRequest_w->setPost(mBodySize);

			// Set Accept-Encoding to allow response compression
			curlEasyRequest_w->setoptString(CURLOPT_ENCODING, mNoCompression ? "identity" : "");
			rv = true;
			break;
		}
		case HTTP_DELETE:
			// Set the handle for an http post
			curlEasyRequest_w->setoptString(CURLOPT_CUSTOMREQUEST, "DELETE");
			rv = true;
			break;

		case HTTP_MOVE:
			// Set the handle for an http post
			curlEasyRequest_w->setoptString(CURLOPT_CUSTOMREQUEST, "MOVE");
			rv = true;
			break;

		default:
			llwarns << "Unhandled URLRequest action: " << mAction << llendl;
			break;
		}
		if(rv)
		{
			curlEasyRequest_w->setopt(CURLOPT_SSL_VERIFYPEER, gNoVerifySSLCert ? 0L : 1L);
			// Don't verify host name if this is not an authentication request,
			// so urls with scrubbed host names will work (improves DNS performance).
			curlEasyRequest_w->setopt(CURLOPT_SSL_VERIFYHOST, (gNoVerifySSLCert || !mIsAuth) ? 0L : 2L);
			curlEasyRequest_w->finalizeRequest(mURL, mResponder->getHTTPTimeoutPolicy(), this);
		}
	}
	return rv;
}
