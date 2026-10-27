#include "System/Exception_handle.h"

ExceptionHandle::ExceptionHandle(int line, const char* file):
	line(line),file(file)
{
}

const char* ExceptionHandle::what() const
{
	std::ostringstream oss;
	oss << GetType() << std::endl << GetOriginString();
	whatBuffer = oss.str();

	return whatBuffer.c_str();
}

const char* ExceptionHandle::GetType() const
{
	return nullptr;
}

int ExceptionHandle::GetLine() const
{
	return 0;
}

const std::string& ExceptionHandle::GetFile() const
{
	return file;
}

std::string ExceptionHandle::GetOriginString() const
{
	std::ostringstream oss;
	oss << "[File]" << file << std::endl << "[Line]" << line;
	return oss.str();
}
