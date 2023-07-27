# slsfs-setup

## Multi Node Cluster Setup

### STEPS to perform on all nodes

#### Prerequisites
- Ubuntu (preferably 22.04)

#### Install Docker
- To install docker on your machine. You can follow the steps [here](https://docs.docker.com/engine/install/ubuntu/) or use the script `install_docker.sh`. 

- use `docker run hello-world` to check if docker installation went through successfully 

**Note**: If you ran the `install_docker` script, you need to log out and log back in to reflect latest changes.

#### Install JAVA
- Use the script `install_java.sh` to install the openJDK latest java version
- Run `sudo update-alternatives --config java` to list java installations
- Run `sudo vi /etc/environments`and set JAVA_HOME to the output from previous command as follows: `JAVA_HOME="/usr/lib/jvm/java-11-openjdk-amd64"`

#### Install Openwhisk
- get the enviroment files (called moc) from (here)[vishalvrv9/slsfs-setup/moc].
- run the `openwhisk_install` script to retrieve pre-configured openwhisk. (NOTE: The rest  of the script is expected to fail at this point)
- go to `/openwhisk/ansible/environments/moc`and edit the host file with hosts required for invokers, schedulers, kafkas, etc.
- run the openwhisk script again to successfully install openwhisk 


