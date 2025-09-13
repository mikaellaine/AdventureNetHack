#!/bin/bash
#export HACKDIR=/usr/games/lib/nethackdir
export HACKDIR=/home/mikael/AdventureNetHack/dat
mkdir ${HACKDIR}/save
touch ${HACKDIR}/perm
touch ${HACKDIR}/record
touch ${HACKDIR}/logfile
touch ${HACKDIR}/xlogfile
if [[ $1 == "d" ]]; then
    gdb --args ./src/nethack -D
else
    ./src/nethack -D
fi
