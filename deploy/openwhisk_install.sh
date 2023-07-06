export HOME=/home/ubuntu;
export OW=/home/ubuntu/openwhisk;
export ENVIRONMENT=moc;
cd $OW/ansible
USE_SUDO="sudo";

$USE_SUDO docker rm -f -v couchdb;

sudo apt-get install python3-pip
sudo pip install ansible==4.1.0
sudo pip install jinja2==3.0.1
pip3 install docker



echo --------CLONING-FORKED-OPENWHISK--------

git clone https://github.com/vishalvrv9/openwhisk.git

echo ----------RUNNNING-GRADLE-BUILD--------
cd openwhisk
echo --------CURRENT DIR------
echo $PWD
$USE_SUDO ./gradlew distDocker
cd $PWD/ansible
./transfer_images.sh;

echo --------RUNNING-ANSIBLE-PLAYBOOK--------
$USE_SUDO ansible-playbook -i environments/$ENVIRONMENT -u ubuntu setup.yml
$USE_SUDO ansible-playbook -i environments/$ENVIRONMENT -u ubuntu openwhisk.yml -e mode=clean
$USE_SUDO ansible-playbook -i environments/$ENVIRONMENT -u ubuntu invoker.yml -e mode=clean

$USE_SUDO ansible-playbook -i environments/$ENVIRONMENT -u ubuntu couchdb.yml
$USE_SUDO ansible-playbook -i environments/$ENVIRONMENT -u ubuntu initdb.yml
$USE_SUDO ansible-playbook -i environments/$ENVIRONMENT -u ubuntu wipe.yml

$USE_SUDO ansible-playbook -i environments/$ENVIRONMENT -u ubuntu openwhisk.yml




