/*
 *  Phusion Passenger - https://www.phusionpassenger.com/
 *  Copyright (c) 2011-2015 Phusion Holding B.V.
 *
 *  "Passenger", "Phusion Passenger" and "Union Station" are registered
 *  trademarks of Phusion Holding B.V.
 *
 *  See LICENSE file for license information.
 */
#include <Core/Controller.h>

/*************************************************************************
 *
 * Implements Core::Controller methods pertaining buffering the request
 * body.
 *
 *************************************************************************/

namespace Passenger {
namespace Core {

using namespace std;
using namespace boost;


/****************************
 *
 * Private methods
 *
 ****************************/


void
Controller::beginBufferingBody(Client *client, Request *req) {
	TRACE_POINT();
	req->state = Request::BUFFERING_REQUEST_BODY;
	req->bodyChannel.start();
	req->bodyBuffer.reinitialize();
	req->bodyBuffer.stop();
	req->beginStopwatchLog(&req->stopwatchLogs.bufferingRequestBody, "buffering request body");
}

ServerKit::Channel::Result
Controller::whenBufferingBody_onRequestBody(Client *client, Request *req,
	const MemoryKit::mbuf &buffer, int errcode)
{
	TRACE_POINT();

	if (buffer.size() > 0) {
		// Data
		req->bodyBytesBuffered += buffer.size();
		SKC_TRACE(client, 3, "Buffering " << buffer.size() <<
			" bytes of client request body: \"" <<
			cEscapeString(StaticString(buffer.start, buffer.size())) <<
			"\"; " << req->bodyBytesBuffered << " bytes buffered so far");
		req->bodyBuffer.feed(buffer);
		return Channel::Result(buffer.size(), false);
	} else if (errcode == 0 || errcode == ECONNRESET) {
		// EOF
		SKC_TRACE(client, 2, "End of request body encountered");
		req->bodyBuffer.feed(MemoryKit::mbuf());
		if (req->bodyType == Request::RBT_CHUNKED) {
			// The data that we've stored in the body buffer is dechunked, so when forwarding
			// the buffered body to the app we must advertise it as being a fixed-length,
			// non-chunked body.
			const unsigned int UINT64_STRSIZE = sizeof("18446744073709551615");
			SKC_TRACE(client, 2, "Adjusting forwarding headers as fixed-length, non-chunked");
			ServerKit::Header *header = (ServerKit::Header *)
				psg_palloc(req->pool, sizeof(ServerKit::Header));
			char *contentLength = (char *) psg_pnalloc(req->pool, UINT64_STRSIZE);
			unsigned int size = integerToOtherBase<boost::uint64_t, 10>(
				req->bodyBytesBuffered, contentLength, UINT64_STRSIZE);

			psg_lstr_init(&header->key);
			psg_lstr_append(&header->key, req->pool, "content-length",
				sizeof("content-length") - 1);
			psg_lstr_init(&header->origKey);
			psg_lstr_append(&header->origKey, req->pool, "Content-Length",
				sizeof("Content-Length") - 1);
			psg_lstr_init(&header->val);
			psg_lstr_append(&header->val, req->pool, contentLength, size);

			header->hash = HashedStaticString("content-length",
				sizeof("content-length") - 1).hash();

			req->headers.erase(HTTP_TRANSFER_ENCODING);
			req->headers.insert(&header, req->pool);
		}
		req->endStopwatchLog(&req->stopwatchLogs.bufferingRequestBody);
		checkoutSession(client, req);
		return Channel::Result(0, true);
	} else {
		const unsigned int BUFSIZE = 1024;
		char *message = (char *) psg_pnalloc(req->pool, BUFSIZE);
		int size = snprintf(message, BUFSIZE,
			"error reading request body: %s (errno=%d)",
			ServerKit::getErrorDesc(errcode), errcode);
		disconnectWithError(&client, StaticString(message, size));
		return Channel::Result(0, true);
	}
}


} // namespace Core
} // namespace Passenger