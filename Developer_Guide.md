# ODF API 建置教學


## ODF API URL 說明

### Introduction for Merge-to 

#### API Interface
* POST  
    * URL : http://localhost:9980/lool/merge-to/[filename in runTimeData (or UUID)]/templates)
    * Description : post data and get the result of the ODF file (odt/ods)
* GET 
    * URL : http://localhost:9980/lool/merge-to/[filename in runTimeData/templates (or UUID)]/api 
    * Description : Get the API Information

Curl Post Example

```
cd runTimeData
curl -X POST http://localhost:9980/lool/merge-to/sheet_example -H "Content-Type:application/json" --data "@sheet_example.json" > sheet_example.ods
```

## Environment Setup

Update your centos
```
yum update -y
sudo systemctl disable firewalld.service
```

安裝編譯與程式所需之套件
```
sudo yum install -y sqlite-devel libcurl-devel cppunit-devel libcap-devel libtool libpng-devel gcc-c++ wget curl
```

install node & npm
```
sudo yum install -y epel-release
sudo yum install -y nodejs
```

install pip & polib
```
sudo yum install -y epel-release
sudo yum install python-pip
```

install jake
```
sudo npm install -g jake
```


安裝專案使用之自編譯套件
```
cd runTimeData
wget https://cloud.ossii.com.tw/index.php/s/6oZMTJoRpYLZT2G/download -O poco.zip --no-check-certificate 
unzip poco.zip
cd poco
sudo yum localinstall -y *.rpm
```

安裝 oxoffice/libreoffice-6.2

```
略, 此版本用國發會之版本
```

## 編譯與環境配置


Configure argument is for developing purpose
```
./autogen.sh
./configure --with-lo-path=/opt/ndcodfsys1 --enable-ssl --with-max-documents=4096 --with-max-connections=4096 --enable-debug --with-lokit-path=bundled/include/
sudo mkdir /etc/loolwsd
sudo cp etc/*.pem /etc/loolwsd/
sudo mkdir -p /usr/local/var/cache/loolwsd
sudo chown `whoami`.root /usr/local/var/cache/loolwsd
sudo chown `whoami`.`whoami` -R `pwd`
make -j4
```

## 執行

一般執行

```
make run
```


執行除錯用腳本

此腳本包含
1. 編譯
2. 複製 .so 檔案
3. 執行除錯用的程式
```
./gdbrun.sh
```

本地執行測試相關檔案說明
* runTimeData : 本地執行所需之檔案都在裡面
* runTimeData/mergeodf.sqlite API 存取紀錄 
* runTimeData/templates : 放置範本 .ot[ts] 的目錄,如果有在 ./configure 下 enable-debug 就會被 include 到 template 的解析路徑
* runTimeData/*.json ：範本對應的 API JSON Data 

執行測試步驟
```
./gdbrun

open new Terminal
cd runTimeData

Writer
curl -X POST http://localhost:9980/lool/merge-to/price_eng_multi -H "Content-Type:application/json" --data "@price_eng_multi.json" > price_eng_multi.odf

Calc
curl -X POST http://localhost:9980/lool/merge-to/sheet_example -H "Content-Type:application/json" --data "@sheet_example.json" > sheet_example.ods

```


## 完整清除環境

```
git clean -f -X 
git clean -f -d -x
```


## Debug with GDB

注意 For Developers who's native language is chinese
* 如果用 cgdb 的話,中文無法正常使用 print, display 的指令輸出
* gdb 如果不使用 TUI 模式(ctrl + x -> ctrl + a ) 就不會有中文顯示問題

安裝 gdb 套件
```
sudo yum install gdb cgdb -y
```


安裝 gdb 所需之額外套件,可先執行一個步驟,再透過 gdb 給的提示安裝即可
```
sudo yum install yum-utils -y
sudo debuginfo-install -y expat-2.1.0-10.el7_3.x86_64 keyutils-libs-1.5.8-3.el7.x86_64 krb5-libs-1.15.1-37.el7_6.x86_64 libattr-2.4.46-13.el7.x86_64 libcap-2.22-9.el7.x86_64 libcom_err-1.42.9-13.el7.x86_64 libgcc-4.8.5-36.el7_6.2.x86_64 libpng-1.5.13-7.el7_2.x86_64 libselinux-2.5-14.1.el7.x86_64 libstdc++-4.8.5-36.el7_6.2.x86_64 openssl-libs-1.0.2k-16.el7_6.1.x86_64 pcre-8.32-17.el7.x86_64 sqlite-3.7.17-8.el7.x86_64 zlib-1.2.7-18.el7.x86_64
```

透過 ps 找出 loolwsd 執行的 pid，再透過 cgdb 掛上去
需要使用 sudo 來提供 loolwsd 有 capabilities set
並且要在專案的根目錄底下執行 cgdb
```
ps -axf | grep lool
sudo cgdb loolwsd 
```

gdb 常用指令

```
設定中斷點
b filename:lineNumber

印出變數
print variable-name

每執行一步都印出變數
display variable-name

清除常態顯示之變數
undisplay variable-name
undisplay     (清除全部)

列出目前的 Called stack
bt [number] : the number define how many latest stacks would show.

Next not into 
n

Step into
s

執行上一個指令
enter

使 gdb 會追進去 child thread
set follow-fork-mode child

使 gdb 只追蹤 root thread(主程序)
set follow-fork-mode parent

連上＆離開程序
attach pid 
detach

列出現在的所有執行緒
info thread

進入特定執行緒
thread thread-number
```

cgdb 常用指令
```
切換不同模式
ESC : 進到原始碼模式
i   : 進到 gdb 模式


[原始碼模式]
設定/取消 break point
SPACE : 在 source code 要中斷的行數按下按下即可，再按一下則取消

o     : 列出所有原始碼(已經 link )
如果有使用 dlopen 等執行期才連結的函式庫,需要先進 gdb 上去讓程式執行到 dlopen  過後,退出 gdb,在重新掛上 gdb 就能夠找到相關的原始碼
並且 cgdb 必須要在 .so 檔的目錄執行才可以把讀取到 .so 的原始碼

cgdb 跟 vim 的操作有一些類似的地方
:q 離開
/ 搜尋
```
