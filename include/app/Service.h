#pragma once
#include <string>
namespace service {
bool start(std::string& error);
void stop();
bool running();
} // namespace service
