# NDCODFAPI Project

Before talking about how to setup this project, something you should know:
* This project is developed by OSSII, and Taiwan National Development Council provides support.
* This project is only builded and test under CentOS-7-1811, so we suggest that you should use centos to ensure the user experience.
* This project requires ndcodfsys, which is an alias of LibreOffice and also developed by OSSII, as an engine to deal the document transform.
* This project was based on LibreOffice-online.

## Pre-required System Environment Setup

Dependecy Package: http://free.nchc.org.tw/ndc.odf/ndcodfapi/ndcodfapi-server-20190422.zip

```
wget http://free.nchc.org.tw/ndc.odf/ndcodfapi/ndcodfapi-server-20190422.zip
unzip ndcodfapi-server-20190422.zip
cd ndcodfapi-server-20190422
cd poco
sudo yum localinstall -y *.rpm
cd ..
cd ndcodfsys
sudo yum localinstall -y *.rpm
```
These RPMs will be installed under path /opt.
Then do not install ndcodfapi-server, which is the old version.


## How-to Use this Project

Assume you have already install the dependency RPM,then let's move on.

There are two way to experience this project:
1. Directly install the released RPM
2. Build in debug mode

### Install released RPM 


First, download the newest RPM
```
sudo yum localinstall -y ndcodfapi-1.1.4-1.x86_64.rpm
```

Second, look around the config file: /etc/loolwsd/loolwsd.xml
There is only one thing you should do is to add your server_name to loolwsd.xml
For example, fill <server_name> with 192.168.3.11:9980 

Last, this API transform templates(ots/odt) into odt/ods
So you may need to put the this project's runTimeData/templates/*.ot[s,t] to this path: /usr/share/NDCODFAPI/ODFReport/templates/

If you finish all the step, then you can try to use curl to get/post.

Command example

```
cd runTimeData

Writer
curl -X POST http://localhost:9980/lool/merge-to/price_eng_multi -H "Content-Type:application/json" --data "@price_eng_multi.json" > price_eng_multi.odf

Calc
curl -X POST http://localhost:9980/lool/merge-to/sheet_example -H "Content-Type:application/json" --data "@sheet_example.json" > sheet_example.ods

```

### Build, Compile, Run

So the second is to build the project hand by hand.If you're developer, the "Developer_Guide.md" may be helpful.
But don't worry if you just want to run for fun. You can use this script "install_build_require.sh" to setup whole environment.

Install the Build-require tools
```
./install_build_require.sh
```

autogen & configure
```
./autogen.sh
./configure --with-lo-path=/opt/ndcodfsys1 --enable-ssl --with-max-documents=4096 --with-max-connections=4096 --enable-debug --with-lokit-path=bundled/include/ --with-poco-includes=/opt/poco/include --with-poco-libs=/opt/poco/lib
```

compile & run
```
make -j4
make run
```

If you come so far to here, it means you are ready to play round this project.

Command example
```
cd runTimeData

Writer
curl -X POST http://localhost:9980/lool/merge-to/price_eng_multi -H "Content-Type:application/json" --data "@price_eng_multi.json" > price_eng_multi.odf

Calc
curl -X POST http://localhost:9980/lool/merge-to/sheet_example -H "Content-Type:application/json" --data "@sheet_example.json" > sheet_example.ods
```

## Further More

In this project, it contain all you need to work around. However, it is not standalone, there are still two other projects you may need to know:

1. [ndcodfapi web manage](https://github.com/NDCODF/ndcodfapi_web) : This is a xoops website to upload your templates and manage them.
2. [ndcodfapi client template assistant](https://github.com/NDCODF/ReportAssistant) : This is the tool to design this project's format templates.

Besides this project, the other projects are aim to setup a workflow to make the odfapi more convenien. So if you want to have a whole experience about ndcodfapi work flow, it's necessary to use the other projects.
