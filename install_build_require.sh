sudo yum update -y
sudo systemctl disable firewalld.service
sudo yum install -y sqlite-devel libcurl-devel cppunit-devel libcap-devel libtool libpng-devel gcc-c++
sudo yum install -y epel-release
sudo yum install -y nodejs
sudo yum install -y python-pip
sudo npm install -g jake
sudo pip install polib
