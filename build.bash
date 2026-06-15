#!/bin/bash

gcc -std=c11 -Wall -Wextra -D_CRT_SECURE_NO_WARNINGS main.c -o dda_window.exe -ld3d11 -ldxgi -ldxguid -luser32 -lgdi32 -ld3dcompiler
