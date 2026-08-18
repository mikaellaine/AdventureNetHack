#!/bin/bash
export HACKDIR=/home/mikael/repo/AdventureNetHack/playground
if [[ $1 == "d" ]]; then
    gdb --args ${HACKDIR}/nethack -D
else
    ${HACKDIR}/nethack -D
fi
