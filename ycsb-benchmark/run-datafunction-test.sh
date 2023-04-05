
hosts=(ow-invoker-1 ow-invoker-2 ow-invoker-3 ow-invoker-4 ow-invoker-5 ow-invoker-6 ow-invoker-7 ow-invoker-8 ow-invoker-9 ow-invoker-10 ow-invoker-11 ow-invoker-12 ow-invoker-13 ow-invoker-14 ow-invoker-15 ow-invoker-16)

for h in "${hosts[@]}"; do
    ssh $h rm -f /tmp/test-storage;
    scp ../functions/datafunction/tools/ssbd.json $h:/tmp/ &
    scp ../functions/datafunction/test-storage $h:/tmp/ &
done

wait < <(jobs -p);

for h in "${hosts[@]}"; do
    ssh "$h" "bash -c 'ulimit -n 8192; /tmp/test-storage 10000 < /tmp/ssbd.json'" &
done
wait < <(jobs -p);
