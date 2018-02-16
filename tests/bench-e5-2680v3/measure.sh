#!/bin/bash

HIT=${HIT:-99}
COST=${COST:-100}
SIZE=${SIZE:-3200}
KEYS=$((SIZE*100/HIT))

# $1: nbthreads
# $2: cpumask
run() {
        for m in 1 2 3 4 5 6 7 8; do for i in {1..10}; do echo -n "$m $i "; taskset -c $2 ../lrubench -t $1 -m $m -k $KEYS -s $SIZE -c $COST | grep ^Glob; done; done | awk '{if (last && $1 != old) {printf "%d %.3f\n",old,tot/last;tot=0;} old=$1; last=$2; tot+=$9+0;} END{printf "%d %.3f\n",old,tot/last}'
}

for thr in {1..12}; do
        run $thr 0-$((thr-1))  > ${thr}c1t.out
done

for thr in {1..12}; do
        run $((thr*2)) 0-$((thr-1)),24-$((24+thr-1))  > ${thr}c2t.out
done

for i in {1..8}; do echo $(grep ^$i {12..1}c1t.out {1..12}c2t.out |cut -f2 -d' ') > perf-$i.out;done

for i in {12..1}c1t.out {1..12}c2t.out; do echo -n "${i%.out} ";echo $(cut -f2 -d' ' $i);done > perf.out
