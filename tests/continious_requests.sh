#!/bin/bash

for i in {1..100}; do 
	curl 192.168.1.114 &
	sleep 0.05  
done