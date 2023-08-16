# slsfs-setup

## Multi Node Cluster Setup

### STEPS to perform on all nodes

#### Prerequisites
- Ubuntu (preferably 22.04)

### Install Docker
- To install docker on your machine. You can follow the steps [here](https://docs.docker.com/engine/install/ubuntu/) or use the script `install_docker.sh`. 

- use `docker run hello-world` to check if docker installation went through successfully 

**Note**: If you ran the `install_docker` script, you need to log out and log back in to reflect latest changes.

### Install JAVA
- Use the script `install_java.sh` to install the openJDK latest java version
- Run `sudo update-alternatives --config java` to list java installations
- Run `sudo vi /etc/environments`and set JAVA_HOME to the output from previous command as follows: `JAVA_HOME="/usr/lib/jvm/java-11-openjdk-amd64"`

### Install Openwhisk

- get the enviroment files (called moc) from (here)[vishalvrv9/slsfs-setup/moc].
- run the `openwhisk_install` script to retrieve pre-configured openwhisk. (NOTE: The rest  of the script is expected to fail at this point)
- go to `/openwhisk/ansible/environments/moc`and edit the host file with hosts required for invokers, schedulers, kafkas, etc.
- run the openwhisk script again to successfully install openwhisk

### Setup a Docker Overlay Network

A Docker overlay network would be required to establish communication between standalone containers on different Docker daemons using an overlay network.

[This Offical Docker Tutorial](https://docs.docker.com/network/network-tutorial-overlay/#use-an-overlay-network-for-standalone-containers) helps achieve that. 


#### Tutorial for setting up 3 node version of slsfs with openwhisk

- In the 3 node version, we setup a openwhisk cluster with 1 controller and 2 invoker nodes.
- To configure the cluster, we use the hosts file present in this folder.
- Update the IP/hosts with the IP/hosts of the different components of the cluster as required. In the example hosts file, the controller is setup as the

##### Setup SLSFS Proxy

- Build the proxy
```
cd ~/slsfs/proxy
make from-docker
```
- Build ssbd
```
cd ~/slsfs/ssbd
make from-docker
```
- Build datafunction
```
cd ~/slsfs/functions/datafunction
make from-docker
```
- Start proxy (to change configurations of proxy before starting, look at start-proxy-args-template.sh)
```
~/slsfs/ycsb-benchmark
./start-proxy-args.sh
```

- Run the ycsb-benchmark
```
cd ~/slsfs/ycsb-benchmark
# to run direct data function test
./run-slsfs-client-direct-test.sh
# to run dynamic proxy test
./run-slsfs-client-dynamic-test.sh
```

#### Changing benchmark configurations

The above scripts run different pre-configured benchmarks. In order to change the proxy configurations, we can tweak the ```start-proxy-args.sh```. The different variables available are:

- EACH_CLIENT_ISSUE : The number of requests each client issues
- TOTAL_CLIENT : Total number of clients
- TOTAL_TIME_AVAILABLE : Time until the client is up and running
- ENABLE_DIRECT_CONNECT : To enable direct connection between client and datafunction
- POLICY_FILETOWORKER : How the proxy assigns datafunction to client
- ENABLE_CACHE : To enable caching 

