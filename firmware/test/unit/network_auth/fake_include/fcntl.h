#pragma once

#define F_GETFL 1
#define F_SETFL 2
#define O_NONBLOCK 0x04

int fcntl(int file_descriptor, int command, ...);
