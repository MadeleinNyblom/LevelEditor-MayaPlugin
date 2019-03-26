#ifndef COMLIB_H
#define COMLIB_H
#include <stdio.h>
#include <iostream>
#include <sstream>
#include <Windows.h>

enum MSG_TYPE { NORMAL, DUMMY};

enum TYPE { PRODUCER, CONSUMER };

struct Header
{
	MSG_TYPE type;
	size_t length;
};

class ComLib 
{
private:
	
	HANDLE hFileMap;
	void* mData;
	bool exists = false;
	unsigned int mSize = 1 << 20;
	
	TYPE userType;

	char* base;
	size_t* head;
	size_t* tail;
	char* memStart;
	size_t bufferSize;

	size_t getFreeMem(size_t tempTail);

public: 

	ComLib(const std::string& secret, const size_t& buffSize, TYPE type);
	bool send(const void*msg, const size_t length);

	bool recv(char* msg, size_t &length);
	char* recvBuffer;
	size_t sizeOfRecvBuffer = 4;

	size_t nextSize();

	~ComLib();
};
#endif



