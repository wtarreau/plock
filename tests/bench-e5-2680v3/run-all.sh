#!/bin/bash

for cost in 100 30 300; do
  for i in 99 98 95 90 80 50; do
    echo cost=$cost hit=$i
    COST=$cost HIT=$i time bash ./measure.sh
    mkdir hit$i-cost$cost
    mv *.out hit$i-cost$cost/
  done
done &
disown
