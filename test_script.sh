#!/bin/bash

POINTS=0
MAXPOINTS=0

COUNTER=0

for WINDOW in `seq 1 100`
do
    for SEED in `seq 1 5`
    do
    	printf "Status update: Started the test with WINDOW = ${WINDOW} and SEED = ${SEED} \n"
        OUTPUT=$(./tester --seed $SEED -w $WINDOW ./reliable | grep -i "SUMMARY")
        SCORE=$(echo "${OUTPUT}" | sed 's/\/14//' | sed 's/[^0-9/]*//g')
        echo "Window: ${WINDOW} SEED: ${SEED} SCORE: ${SCORE}" >> outreliable.txt
        POINTS=$((POINTS+SCORE))
        MAXPOINTS=$((MAXPOINTS + 14))
        COUNTER=$((COUNTER+1))
        clear
        printf "Current percentage: %.2f" $((10000 *POINTS/MAXPOINTS*100))e-4
        echo " with ${COUNTER} TESTS"
    done
done


