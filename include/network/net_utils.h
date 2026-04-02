#pragma once
#include <string>
#include <vector>
#include <cstddef>
#include "common/flexql_protocol.h"

using namespace std;

bool Net_Send_All(int fd, const void* buf, size_t len);
bool Net_Receive_All(int fd, void* buf, size_t len);
bool Net_Send_Message(int fd, uint32_t type, const string& payload);
string Net_Pack_Fields(const vector<string>& fields);
vector<string> Net_Unpack_Fields(const string& payload);