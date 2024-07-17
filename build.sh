#!/bin/sh

rm -f config.h
sudo make clean install
sudo make clean
rm -f config.h
