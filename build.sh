#!/bin/sh

c++ -std=c++20 -O3 dedup.cpp -o dedup -Wl,-stack_size,0x800000
