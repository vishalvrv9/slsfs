docker run --rm --entrypoint cat hare1039/transport:0.0.2 /bin/slsfs-client > slsfs-client

chmod +x slsfs-client

hosts=(ow-invoker-1 ow-invoker-2 ow-invoker-3 ow-invoker-4 ow-invoker-5 ow-invoker-6 ow-invoker-7 ow-invoker-8 ow-invoker-9 ow-invoker-10 ow-invoker-11 ow-invoker-12 ow-invoker-13 ow-invoker-14 ow-invoker-15);
hosts=(ow-invoker-1);

for h in "${hosts[@]}"; do
    scp slsfs-client "$h": &
done
wait < <(jobs -p);

echo starting;

for h in "${hosts[@]}"; do
    ssh "$h" "bash -c '/home/ubuntu/slsfs-client 1000 1 4096 /tmp/result.txt'" &
done
wait < <(jobs -p);
