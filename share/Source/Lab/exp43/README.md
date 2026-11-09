# 実験4-3：授業資料のファイル転送を拡張する

対応資料：ネットワークフォルダの「（実験資料）NetLab2020_20261001.pdf」スライド69〜75。

サーバーは指定ファイルをopenし、readしたデータをソケットへwriteします。
クライアントは要求を送り、ソケットからreadしたデータをファイルへwriteします。
サーバーが送信後に接続を閉じ、クライアントのreadが0になったら終了します。
ファイルサイズを通知する独自のヘッダーは使いません。

計測用のコードは両方ともmainの中にあり、STEPコメントの順に処理を追えます。
部分書き込みを処理する内側のwhileは、readで得たnバイトを最後までwriteするためです。

## ビルド

実験用Dockerノードで実行します。

```sh
cd /home/ubuntu/share/Source/Lab/exp43
make
make test.dat
wc -c test.dat
```

1MiB（1,048,576バイト）のファイルを作ります。すでにtest.datがあれば上書きしません。
ヘッダーのBUF_LENは最初1000です。実験3や4-1のヘッダーとは別ファイルです。

```text
./fserver.out filename [port]
./fclient.out host port output_file [wait_us]
```

サーバーのポートは省略すると10000です。wait_usは省略すると0、1000なら1msです。
同じノードでの確認にはlocalhost、別ノードへ接続する場合はサーバーの共有LAN側のIPを使います。

## A・B：時間とグッドプット

ターミナル1：

```sh
./fserver.out test.dat
```

ターミナル2：

```sh
./fclient.out localhost 10000 received-normal.dat
cmp test.dat received-normal.dat
```

サーバーは次の接続を待つため動き続けます。停止はCtrl+Cです。
cmpが出力なし・終了コード0なら、送信元と受信先のバイト列が一致しています。

| 表示 | 意味 |
|---|---|
| Send bytes | サーバーがファイルから読み、送信した総バイト数 |
| File read count | ファイルからデータを読み込んだreadの回数 |
| Send count | ファイル本体をソケットへ書いたwriteの回数 |
| Receive bytes | クライアントが受信・保存した総バイト数 |
| Receive count | ファイル本体を1バイト以上受信したreadの回数 |
| Time | 要求送信直前から、受信終了を検出して保存先をcloseするまでの秒数 |
| Goodput | Receive bytes × 8 ÷ Time。bpsとMbpsで表示 |

時刻は資料と同じCLOCK_REALTIMEで取得します。
名前解決、接続確立、出力ファイルのopenは計測開始前です。
要求の送信・サーバーの処理待ち・データ受信・ファイル書き込み・受信処理の待ち時間は含みます。
Timeはクライアントの処理全体の時間で、ネットワーク上だけの時間ではありません。
ディスクへの永続化を強制する処理は入れていません。

バイト数は各readの戻り値nを足します。BUF_LEN×回数では計算しません。
最後のreadがBUF_LENより短い場合も、正しい総量を計算できます。

## C：BUF_LENを小さくする

受信側の条件を固定するため、最初にBUF_LEN=1000でmakeしておきます。
通常条件の結果を記録したらサーバーをCtrl+Cで止め、
このフォルダのicslab2_net.hにある1行を変更します。

```c
#define BUF_LEN 100
```

その後、サーバーだけを再コンパイルします。

```sh
make fserver.out
./fserver.out test.dat
```

クライアントは先ほど1000でコンパイルしたものを使います。

```sh
./fclient.out localhost 10000 received-100.dat
cmp test.dat received-100.dat
```

同じ手順でBUF_LEN=10でも測定します。受信ファイル名も変更してください。
サーバーの待受表示に実際にコンパイルされたBUF_LENが出ます。
ここでmakeだけを実行するとクライアントも小さいBUF_LENで再コンパイルされるため、
送信側だけを変える比較ではmake fserver.outを指定してください。
両方のBUF_LENを変える実験もできますが、その場合は条件に両方の値を記録します。

**BUF_LENはアプリが1回に読み書きする最大量です。TCPセグメントの実際のサイズとは同一ではありません。**
TCPはバイトストリームなので、複数のwriteをまとめたり、1回のwriteを分割してreadが返ったりします。
実際のパケットはtcpdumpで確認します。

## D：受信処理にウエイトを入れる

サーバーを停止し、icslab2_net.hをBUF_LEN=1000に戻してmakeします。

```sh
make
./fserver.out test.dat
```

別ターミナルで、0μsと1000μsを比較します。

```sh
./fclient.out localhost 10000 received-wait0.dat 0
./fclient.out localhost 10000 received-wait1000.dat 1000
cmp test.dat received-wait0.dat
cmp test.dat received-wait1000.dat
```

クライアントはreadでデータを受け取るたび、ファイルへ書く前にusleep(wait_us)します。
この待ちによって次のreadも遅くなります。受信待ちを入れた条件のTimeには、この待ち時間も含まれます。

各転送でサーバーのSend countとクライアントのReceive countを対応させて記録します。
終了を返すread=0、要求メッセージのwrite/readは回数に含めていません。

## ノード間の計測とパケット観察

例はnode1をサーバー、node2をクライアントにし、現在のdocker-compose.ymlのlan1-2を使う場合です。

node1：

```sh
cd /home/ubuntu/share/Source/Lab/exp43
./fserver.out test.dat
```

node1の別ターミナルでキャプチャを開始：

```sh
cd /home/ubuntu/share/Source/Lab/exp43
mkdir -p results
sudo tcpdump -i any -nn -s 0 -w results/wait1000.pcap 'tcp port 10000'
```

node2：

```sh
cd /home/ubuntu/share/Source/Lab/exp43
./fclient.out 172.20.0.10 10000 received-node2.dat 1000
cmp test.dat received-node2.dat
```

キャプチャ側をCtrl+Cで止めた後：

```sh
sudo tcpdump -nn -tttt -r results/wait1000.pcap
```

Cも通常・100・10で別のpcapへ保存します。
データのlength、ACK、ウィンドウ、送受信の間隔を比較してください。
ウィンドウを数値で読む場合はTCPのウィンドウスケーリングにも注意します。

shareは全ノードで同じホストフォルダを共有します。受信先には送信元と別名を指定します。
localhostの結果はノード間の帯域・遅延の測定値として扱いません。

## 記録する表

| 条件 | 送信BUF_LEN | 受信BUF_LEN | wait_us | 受信bytes | Time [s] | Goodput [Mbps] | Send count | Receive count | cmp | 観察 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|---|
| 通常 | 1000 | 1000 | 0 | | | | | | | |
| 小さい送信 | 100 | 1000 | 0 | | | | | | | |
| さらに小さい送信 | 10 | 1000 | 0 | | | | | | | |
| 遅い受信 | 1000 | 1000 | 1000 | | | | | | | |

ファイル内容の正しさはcmpで確認します。
授業資料と同じ接続終了による方式なので、接続が正常終了しただけでは予定ファイルサイズまで受信した証明にはなりません。
途中でサーバー側のファイル読み込みが失敗した場合などは、サーバーのエラー表示とcmpも確認します。

以前のcompare_local.py、common.h、独自のサイズ通知は、この教材に沿う版では使いません。
make compareもありません。条件の変更と結果記録を上の手順で行います。
