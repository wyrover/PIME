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

#include "PipeServer.h"
#include <Windows.h>
#include <ShlObj.h>
#include <Shellapi.h>
#include <Lmcons.h> // for UNLEN
#include <iostream>
#include <cstring>
#include <cassert>
#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <algorithm>
#include <codecvt>  // for utf8 conversion
#include <locale>  // for wstring_convert

#include <json/json.h>

#include "BackendServer.h"
#include "Utils.h"

using namespace std;

static wstring_convert<codecvt_utf8<wchar_t>> utf8Codec;

namespace PIME {

PipeServer* PipeServer::singleton_ = nullptr;

PipeServer::PipeServer() :
	securittyDescriptor_(nullptr),
	acl_(nullptr),
	everyoneSID_(nullptr),
	allAppsSID_(nullptr),
	pendingPipeConnection_(false),
	quitExistingLauncher_(false) {
	// this can only be assigned once
	assert(singleton_ == nullptr);
	singleton_ = this;
}

PipeServer::~PipeServer() {
	if (connectPipeOverlapped_.hEvent != INVALID_HANDLE_VALUE)
		CloseHandle(connectPipeOverlapped_.hEvent);

	if (everyoneSID_ != nullptr)
		FreeSid(everyoneSID_);
	if (allAppsSID_ != nullptr)
		FreeSid(allAppsSID_);
	if (securittyDescriptor_ != nullptr)
		LocalFree(securittyDescriptor_);
	if (acl_ != nullptr)
		LocalFree(acl_);
}

void PipeServer::initBackendServers(const std::wstring & topDirPath) {
	// load known backend implementations
	Json::Value backends;
	if (loadJsonFile(topDirPath + L"\\backends.json", backends)) {
		if (backends.isArray()) {
			for (auto it = backends.begin(); it != backends.end(); ++it) {
				auto& backendInfo = *it;
				BackendServer* backend = new BackendServer(this, backendInfo);
				backends_.push_back(backend);
			}
		}
	}

	// maps language profiles to backend names
	initInputMethods(topDirPath);
}

void PipeServer::initInputMethods(const std::wstring& topDirPath) {
	// maps language profiles to backend names
	for (BackendServer* backend : backends_) {
		std::wstring dirPath = topDirPath + L"\\" + utf8Codec.from_bytes(backend->name_) + L"\\input_methods";
		// scan the dir for lang profile definition files (ime.json)
		WIN32_FIND_DATA findData = { 0 };
		HANDLE hFind = ::FindFirstFile((dirPath + L"\\*").c_str(), &findData);
		if (hFind != INVALID_HANDLE_VALUE) {
			do {
				if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { // this is a subdir
					if (findData.cFileName[0] != '.') {
						std::wstring imejson = dirPath;
						imejson += '\\';
						imejson += findData.cFileName;
						imejson += L"\\ime.json";
						// Make sure the file exists
						DWORD fileAttrib = GetFileAttributesW(imejson.c_str());
						if (fileAttrib != INVALID_FILE_ATTRIBUTES) {
							// load the json file to get the info of input method
							Json::Value json;
							if (loadJsonFile(imejson, json)) {
								std::string guid = json["guid"].asString();
								transform(guid.begin(), guid.end(), guid.begin(), tolower);  // convert GUID to lwoer case
																							 // map text service GUID to its backend server
								backendMap_.insert(std::make_pair(guid, backendFromName(backend->name_.c_str())));
							}
						}
					}
				}
			} while (::FindNextFile(hFind, &findData));
			::FindClose(hFind);
		}
	}
}

void PipeServer::finalizeBackendServers() {
	// try to terminate launched backend server processes
	for (BackendServer* backend : backends_) {
		backend->terminateProcess();
		delete backend;
	}
}

BackendServer* PipeServer::backendFromName(const char* name) {
	// for such a small list, linear search is often faster than hash table or map
	for (BackendServer* backend : backends_) {
		if (backend->name_ == name)
			return backend;
	}
	return nullptr;
}

BackendServer* PipeServer::backendFromLangProfileGuid(const char* guid) {
	auto it = backendMap_.find(guid);
	if (it != backendMap_.end())  // found the backend for the text service
		return it->second;
	return nullptr;
}

string PipeServer::getPipeName(const char* base_name) {
	string pipe_name;
	char username[UNLEN + 1];
	DWORD unlen = UNLEN + 1;
	if (GetUserNameA(username, &unlen)) {
		// add username to the pipe path so it will not clash with other users' pipes.
		pipe_name = "\\\\.\\pipe\\";
		pipe_name += username;
		pipe_name += "\\PIME\\";
		pipe_name += base_name;
	}
	return pipe_name;
}

void PipeServer::parseCommandLine(LPSTR cmd) {
	int argc;
	wchar_t** argv = CommandLineToArgvW(GetCommandLine(), &argc);
	// parse command line options
	for (int i = 1; i < argc; ++i) {
		const wchar_t* arg = argv[i];
		if (wcscmp(arg, L"/quit") == 0)
			quitExistingLauncher_ = true;
	}
	LocalFree(argv);
}

// send IPC message "quit" to the existing PIME Launcher process.
void PipeServer::terminateExistingLauncher() {
	string pipe_name = getPipeName("Launcher");
	char buf[16];
	DWORD rlen;
	::CallNamedPipeA(pipe_name.c_str(), "quit", 4, buf, sizeof(buf) - 1, &rlen, 1000); // wait for 1 sec.
}

void PipeServer::quit() {
	finalizeBackendServers();
	ExitProcess(0); // quit PipeServer
}

void PipeServer::handleBackendReply(const std::string clientId, const char* readBuf, size_t len) {
	// find the client with this ID
	auto it = std::find_if(clients_.cbegin(), clients_.cend(), [clientId](const ClientInfo* client) {
		return client->clientId_ == clientId;
	});
	if (it != clients_.cend()) {
		auto client = *it;
		uv_buf_t buf = {len, (char*)readBuf};
		uv_write_t* req = new uv_write_t{};
		uv_write(req, client->stream(), &buf, 1, [](uv_write_t* req, int status) {
			delete req;
		});
	}
}

void PipeServer::initSecurityAttributes() {
	// create security attributes for the pipe
	// http://msdn.microsoft.com/en-us/library/windows/desktop/hh448449(v=vs.85).aspx
	// define new Win 8 app related constants
	memset(&explicitAccesses_, 0, sizeof(explicitAccesses_));
	// Create a well-known SID for the Everyone group.
	// FIXME: we should limit the access to current user only
	// See this article for details: https://msdn.microsoft.com/en-us/library/windows/desktop/hh448493(v=vs.85).aspx

	SID_IDENTIFIER_AUTHORITY worldSidAuthority = SECURITY_WORLD_SID_AUTHORITY;
	AllocateAndInitializeSid(&worldSidAuthority, 1,
		SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0, &everyoneSID_);

	// https://services.land.vic.gov.au/ArcGIS10.1/edESRIArcGIS10_01_01_3143/Python/pywin32/PLATLIB/win32/Demos/security/explicit_entries.py

	explicitAccesses_[0].grfAccessPermissions = GENERIC_ALL;
	explicitAccesses_[0].grfAccessMode = SET_ACCESS;
	explicitAccesses_[0].grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
	explicitAccesses_[0].Trustee.pMultipleTrustee = NULL;
	explicitAccesses_[0].Trustee.MultipleTrusteeOperation = NO_MULTIPLE_TRUSTEE;
	explicitAccesses_[0].Trustee.TrusteeForm = TRUSTEE_IS_SID;
	explicitAccesses_[0].Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
	explicitAccesses_[0].Trustee.ptstrName = (LPTSTR)everyoneSID_;

	// FIXME: will this work under Windows 7 and Vista?
	// create SID for app containers
	SID_IDENTIFIER_AUTHORITY appPackageAuthority = SECURITY_APP_PACKAGE_AUTHORITY;
	AllocateAndInitializeSid(&appPackageAuthority,
		SECURITY_BUILTIN_APP_PACKAGE_RID_COUNT,
		SECURITY_APP_PACKAGE_BASE_RID,
		SECURITY_BUILTIN_PACKAGE_ANY_PACKAGE,
		0, 0, 0, 0, 0, 0, &allAppsSID_);

	explicitAccesses_[1].grfAccessPermissions = GENERIC_ALL;
	explicitAccesses_[1].grfAccessMode = SET_ACCESS;
	explicitAccesses_[1].grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
	explicitAccesses_[1].Trustee.pMultipleTrustee = NULL;
	explicitAccesses_[1].Trustee.MultipleTrusteeOperation = NO_MULTIPLE_TRUSTEE;
	explicitAccesses_[1].Trustee.TrusteeForm = TRUSTEE_IS_SID;
	explicitAccesses_[1].Trustee.TrusteeType = TRUSTEE_IS_GROUP;
	explicitAccesses_[1].Trustee.ptstrName = (LPTSTR)allAppsSID_;

	// create DACL
	DWORD err = SetEntriesInAcl(2, explicitAccesses_, NULL, &acl_);
	if (0 == err) {
		// security descriptor
		securittyDescriptor_ = (PSECURITY_DESCRIPTOR)LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH);
		InitializeSecurityDescriptor(securittyDescriptor_, SECURITY_DESCRIPTOR_REVISION);

		// Add the ACL to the security descriptor. 
		SetSecurityDescriptorDacl(securittyDescriptor_, TRUE, acl_, FALSE);
	}

	securityAttributes_.nLength = sizeof(SECURITY_ATTRIBUTES);
	securityAttributes_.lpSecurityDescriptor = securittyDescriptor_;
	securityAttributes_.bInheritHandle = TRUE;
}


// References:
// https://msdn.microsoft.com/en-us/library/windows/desktop/aa365588(v=vs.85).aspx
void PipeServer::initPipe(const char* app_name) {
	char username[UNLEN + 1];
	DWORD unlen = UNLEN + 1;
	if (GetUserNameA(username, &unlen)) {
		// add username to the pipe path so it will not clash with other users' pipes.
		char pipe_name[MAX_PATH];
		sprintf(pipe_name, "\\\\.\\pipe\\%s\\PIME\\%s", username, app_name);
		// create the pipe
		uv_pipe_init_windows_named_pipe(uv_default_loop(), &serverPipe_, 0, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE, &securityAttributes_);
		serverPipe_.data = this;
		uv_pipe_bind(&serverPipe_, pipe_name);
	}
}


void PipeServer::onNewClientConnected(uv_stream_t* server, int status) {
	auto client = new ClientInfo{this};
	uv_pipe_init_windows_named_pipe(uv_default_loop(), &client->pipe_, 0, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE, &securityAttributes_);
	client->pipe_.data = client;
	uv_stream_set_blocking((uv_stream_t*)&client->pipe_, 0);
	uv_accept(server, (uv_stream_t*)&client->pipe_);
	clients_.push_back(client);

	uv_read_start((uv_stream_t*)&client->pipe_,
		[](uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
			buf->base = new char[suggested_size];
			buf->len = suggested_size;
		},
		[](uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
			auto client = (ClientInfo*)stream->data;
			client->server_->onClientDataReceived(stream, nread, buf);
		}
	);
}

void PipeServer::onClientDataReceived(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
	auto client = (ClientInfo*)stream->data;
	if (nread <= 0 || nread == UV_EOF || buf->base == nullptr) {
		if (buf->base) {
			delete []buf->base;
		}
		return;
	}
	if (buf->base) {
		handleClientMessage(client, buf->base, buf->len);
		delete[]buf->base;
	}
}

int PipeServer::exec(LPSTR cmd) {
	parseCommandLine(cmd);
	if (quitExistingLauncher_) { // terminate existing launcher process
		terminateExistingLauncher();
		return 0;
	}

	// get the PIME directory
	wchar_t exeFilePathBuf[MAX_PATH];
	DWORD len = GetModuleFileNameW(NULL, exeFilePathBuf, MAX_PATH);
	exeFilePathBuf[len] = '\0';

	// Ask Windows to restart our process when crashes happen.
	RegisterApplicationRestart(exeFilePathBuf, 0);

	// strip the filename part to get dir path
	wchar_t* p = wcsrchr(exeFilePathBuf, '\\');
	if (p)
		*p = '\0';
	topDirPath_ = exeFilePathBuf;

	// must set CWD to our dir. otherwise the backends won't launch.
	::SetCurrentDirectoryW(topDirPath_.c_str());

	// this is the first instance
	initBackendServers(topDirPath_);

	// preparing for the server pipe
	initSecurityAttributes();

	// initialize the server pipe
	initPipe("Launcher");

	// listen to events
	uv_listen(reinterpret_cast<uv_stream_t*>(&serverPipe_), 32, [](uv_stream_t* server, int status) {
		PipeServer* _this = (PipeServer*)server->data;
		_this->onNewClientConnected(server, status);
	});
	// run the main loop
	uv_run(uv_default_loop(), UV_RUN_DEFAULT);
}

void PipeServer::handleClientMessage(ClientInfo* client, const char* readBuf, size_t len) {
	// special handling, asked for quitting PIMELauncher.
	if (strcmp("quit", readBuf) == 0) {
		quit();
		return;
	}

	// call the backend to handle this message
	if (client->backend_ == nullptr) {
		// backend is unknown, parse the json to look it up.
		Json::Value msg;
		Json::Reader reader;
		if (reader.parse(readBuf, msg)) {
			const char* method = msg["method"].asCString();
			if (method != nullptr) {
				OutputDebugStringA(method);
				if (strcmp(method, "init") == 0) {  // the client connects to us the first time
					const char* guid = msg["id"].asCString();
					client->backend_ = backendFromLangProfileGuid(guid);
					if (client->backend_ == nullptr) {
						// FIXME: write some response to indicate the failure
						return;
					}
				}
			}
		}
		if (client->backend_ == nullptr) {
			// fail to find a usable backend
			// FIXME: write some response to indicate the failure
			return;
		}
	}
	// pass the incoming message to the backend
	auto backend = client->backend_;
	backend->handleClientMessage(client, readBuf, len);
}

void PipeServer::closeClient(ClientInfo* client) {
	if (client->backend_ != nullptr) {
		// FIXME: client->backend_->removeClient(client->clientId_);
		// notify the backend server to remove the client
		const char msg[] = "{\"method\":\"close\"}";
		client->backend_->handleClientMessage(client, msg, sizeof(msg));
	}

	clients_.erase(find(clients_.begin(), clients_.end(), client));
	uv_close((uv_handle_t*)&client->pipe_, [](uv_handle_t* handle) {
		auto client = (ClientInfo*)handle->data;
		delete client;
	});
}

} // namespace PIME
