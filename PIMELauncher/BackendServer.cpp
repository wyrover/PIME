//
//	Copyright (C) 2015 - 2016 Hong Jen Yee (PCMan) <pcman.tw@gmail.com>
//
//	This library is free software; you can redistribute it and/or
//	modify it under the terms of the GNU Library General Public
//	License as published by the Free Software Foundation; either
//	version 2 of the License, or (at your option) any later version.
//
//	This library is distributed in the hope that it will be useful,
//	but WITHOUT ANY WARRANTY; without even the implied warranty of
//	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
//	Library General Public License for more details.
//
//	You should have received a copy of the GNU Library General Public
//	License along with this library; if not, write to the
//	Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
//	Boston, MA  02110-1301, USA.
//

#include <Windows.h>
#include <ShlObj.h>
#include <Shellapi.h>
#include <Lmcons.h> // for UNLEN
#include <Wincrypt.h>  // for CryptBinaryToString (used for base64 encoding)
#include <cstring>
#include <cassert>
#include <chrono>  // C++ 11 clock functions
#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <algorithm>
#include <json/json.h>

#include "BackendServer.h"
#include "PipeServer.h"

using namespace std;

namespace PIME {

BackendServer::BackendServer(PipeServer* pipeServer, const Json::Value& info) :
	pipeServer_{pipeServer},
	process_{ nullptr },
	stdinPipe_{nullptr},
	stdoutPipe_{nullptr},
	ready_{false},
	name_(info["name"].asString()),
	command_(info["command"].asString()),
	workingDir_(info["workingDir"].asString()),
	params_(info["params"].asString()) {
}

BackendServer::~BackendServer() {
	terminateProcess();
}

void BackendServer::handleClientMessage(ClientInfo * client, const char * readBuf, size_t len) {
	if (!isProcessRunning()) {
		startProcess();
	}

	// message format: <client_id>\t<json string>\n
	string msg = string{ client->clientId_ };
	msg += "\t";
	msg.append(readBuf, len);
	msg += "\n";

	// write the message to the backend server
	uv_buf_t buf = {msg.length(), (char*)msg.c_str()};
	uv_write_t* req = new uv_write_t{};
	uv_write(req, stdinStream(), &buf, 1, [](uv_write_t* req, int status) {
		delete req;
	});
}

void BackendServer::startProcess() {
	process_ = new uv_process_t{};
	process_->data = this;
	// create pipes for stdio of the child process
	stdinPipe_ = new uv_pipe_t{};
	stdinPipe_->data = this;
	uv_pipe_init(uv_default_loop(), stdinPipe_, 0);

	stdoutPipe_ = new uv_pipe_t{};
	stdoutPipe_->data = this;
	uv_pipe_init(uv_default_loop(), stdoutPipe_, 0);

	uv_stdio_container_t stdio_containers[3];
	stdio_containers[0].data.stream = stdinStream();
	stdio_containers[0].flags = uv_stdio_flags(UV_CREATE_PIPE | UV_READABLE_PIPE | UV_WRITABLE_PIPE);
	stdio_containers[1].data.stream = stdoutStream();
	stdio_containers[1].flags = uv_stdio_flags(UV_CREATE_PIPE | UV_READABLE_PIPE | UV_WRITABLE_PIPE);
	stdio_containers[2].data.stream = nullptr;
	stdio_containers[2].flags = UV_IGNORE;

	char full_exe_path[MAX_PATH];
	size_t cwd_len = MAX_PATH;
	uv_cwd(full_exe_path, &cwd_len);
	full_exe_path[cwd_len] = '\\';
	strcpy(full_exe_path + cwd_len + 1, command_.c_str());
	const char* argv[] = {
		full_exe_path,
		params_.c_str(),
		nullptr
	};
	uv_process_options_t options = { 0 };
	options.exit_cb = [](uv_process_t* process, int64_t exit_status, int term_signal) {
		reinterpret_cast<BackendServer*>(process->data)->onProcessTerminated(exit_status, term_signal);
	};
	options.flags = 0; //  UV_PROCESS_WINDOWS_VERBATIM_ARGUMENTS;
	options.file = argv[0];
	options.args = const_cast<char**>(argv);
	char full_working_dir[MAX_PATH];
	::GetFullPathNameA(workingDir_.c_str(), MAX_PATH, full_working_dir, nullptr);
	options.cwd = full_working_dir;
	options.env = nullptr;
	options.stdio_count = 3;
	options.stdio = stdio_containers;
	int ret = uv_spawn(uv_default_loop(), process_, &options);
	if (ret < 0) {
		delete process_;
		process_ = nullptr;
		uv_close(reinterpret_cast<uv_handle_t*>(stdinPipe_), [](uv_handle_t* handle) {
			delete reinterpret_cast<uv_pipe_t*>(handle);
		});
		stdinPipe_ = nullptr;
		uv_close(reinterpret_cast<uv_handle_t*>(stdoutPipe_), [](uv_handle_t* handle) {
			delete reinterpret_cast<uv_pipe_t*>(handle);
		});
		stdoutPipe_ = nullptr;
		return;
	}

	// start receiving data from the backend server
	uv_read_start(stdoutStream(), allocReadBuf,
		[](uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
			reinterpret_cast<BackendServer*>(stream->data)->onProcessDataReceived(stream, nread, buf);
		}
	);
}

void BackendServer::terminateProcess() {
	if (process_) {
		uv_process_kill(process_, SIGTERM);
	}
}

// check if the backend server process is running
bool BackendServer::isProcessRunning() {
	return process_ != nullptr;
}

void BackendServer::allocReadBuf(uv_handle_t *, size_t suggested_size, uv_buf_t * buf) {
	buf->base = new char[suggested_size];
	buf->len = suggested_size;
}

void BackendServer::onProcessDataReceived(uv_stream_t * stream, ssize_t nread, const uv_buf_t * buf) {
	if (nread < 0 || nread == UV_EOF) {
		if (buf->base)
			delete []buf->base;
		return;
	}
	if (buf->base) {
		// initial ready message from the backend server
		if (buf->base[0] == '\0') {
			ready_ = true;
		}
		else {  // pass the response back to the clients
			auto line = buf->base;
			auto buf_end = buf->base + nread;
			while (line < buf_end) {
				if(auto line_end = strchr(line, '\n')) {
					if (auto sep = strchr(line, '\t')) {
						string clientId(line, sep - line);
						auto msg = sep + 1;
						auto msg_len = line_end - msg;
						pipeServer_->handleBackendReply(clientId, msg, msg_len);
					}
					line = line_end + 1;
				}
				else {
					break;
				}
			}
		}
	}
}

void BackendServer::onProcessTerminated(int64_t exit_status, int term_signal) {
	delete process_;
	process_ = nullptr;
	ready_ = false;
	uv_close(reinterpret_cast<uv_handle_t*>(stdinPipe_), [](uv_handle_t* handle) {
		delete reinterpret_cast<uv_pipe_t*>(handle);
	});
	stdinPipe_ = nullptr;
	uv_close(reinterpret_cast<uv_handle_t*>(stdoutPipe_), [](uv_handle_t* handle) {
		delete reinterpret_cast<uv_pipe_t*>(handle);
	});
}

} // namespace PIME
