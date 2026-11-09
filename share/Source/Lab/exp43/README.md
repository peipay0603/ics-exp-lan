# 実験4-3：TCPファイル転送の時間測定

授業資料スライド72〜75のA〜Dを実行するためのプログラムです。
実験4-1の `open` → `read` → `write` を使ったファイル転送を拡張しています。

| 課題 | 実装 |
|---|---|
| A：転送時間 | クライアントの `seconds` |
| B：グッドプット | `payload_bytes * 8 / seconds`。bpsとMbpsを表示 |
| C：小さい送信サイズ | サーバーの `chunk_bytes` 引数で1回のファイル読込・送信上限を変更 |
| D：遅い受信 | クライアントの `wait_us` 引数で、ファイル本体をreadする前に毎回待つ |
| D：送受信回数の比較 | サーバーの `send_calls` とクライアントの `receive_calls` |
| D：パケット観察 | 下記のtcpdump手順で、実験用ノード上のTCPをキャプチャ |

## 1. ビルドとテストデータ作成

Docker内の実験用ノードで実行します。

```sh
cd /home/ubuntu/share/Source/Lab/exp43
make
make test.dat
wc -c test.dat
```

`test.dat` は1,048,576バイト（1MiB）です。計算には実際の転送バイト数を使います。
実験3のソースや実験4-1の `Lab/fserver.c`・`Lab/fclient.c` はそのまま残しています。

## 2. 引数

```text
./fserver.out input_file [port=10000] [chunk_bytes=1000] [max_clients=0]
./fclient.out host port output_file [wait_us=0] [receive_buffer=1000]
```

- `chunk_bytes`：1回にファイルから読み込む最大バイト数。省略時は共通ヘッダーの `BUF_LEN`。
- `wait_us`：ファイル本体の各read直前の待ち時間。1000なら1ms。省略時は0。
- `receive_buffer`：1回のreadの最大バイト数。Cでは1000に固定し、送信サイズだけを変えます。
- `max_clients`：何接続を処理したらサーバーを終了するか。0は継続、1は一度で終了。
- サーバーのポート0は空いているポートを自動選択します。比較スクリプト用です。
- サイズは1〜1,048,576バイト、待ち時間は0〜60,000,000μsの整数で指定します。

## 3. A・B：通常のファイル転送を測る

同じノードのターミナル1：

```sh
./fserver.out test.dat 10000 1000
```

ターミナル2：

```sh
./fclient.out localhost 10000 received.dat 0 1000
cmp test.dat received.dat
```

`cmp` が何も表示せず終了すれば一致しています。サーバーはCtrl+Cで終了できます。

クライアントのRESULT行：

- `payload_bytes`：保存したファイル本体のバイト数。
- `seconds`：接続確立後の要求送信直前から、全ファイル受信・保存先close完了まで。
- `goodput_bps`：ファイル本体の総bit数 / seconds。
- `goodput_mbps`：goodput_bps / 1,000,000。
- `receive_calls`：ファイル本体を1バイト以上受信したreadの回数。EOF・再試行は除外。

サーバーのRESULT行：

- `file_reads`：ファイル本体を読み込んだreadの回数。
- `send_calls`：ファイル本体の送信に成功したwriteの回数。部分書き込み時の追加writeも数える。
- `server_seconds`：サイズ通知前から、全ファイルデータをソケットへ書き終わるまで。

**転送時間とグッドプットの比較にはクライアントのsecondsを使います。**
サーバーのwrite完了は、相手が受信・保存し終えたことを意味しません。
クライアント時間には要求・応答待ち、ファイル作成・書き込み、受信待ち時間も含まれます。
close後のディスクへの永続化（fsync）は計測していません。

授業例と同じ `clock_gettime(CLOCK_REALTIME, ...)` を使用します。
ファイル本体の前に `OK <ファイルサイズ>\n` を通知し、途中切断を検出します。
要求・サイズ通知はファイルへ保存せず、payload_bytes・送受信回数にも含めません。

## 4. C：送信サイズを小さくする

毎回サーバーを終了してから、送信サイズを1000→100→10バイトと変えて実行します。

```sh
./fserver.out test.dat 10000 100
```

別ターミナル：

```sh
./fclient.out localhost 10000 received-100.dat 0 1000
cmp test.dat received-100.dat
```

10バイトの場合はサーバーの第3引数を10にし、出力ファイル名も変えます。
受信バッファ・ネットワーク条件・ファイルは同じにして、転送時間、グッドプット、
send_callsとreceive_callsを記録してください。

**アプリのwriteサイズとTCPセグメントのサイズは同じとは限りません。**
TCPはデータを結合・分割できるため、send_callsとreceive_callsも一致するとは限りません。
資料のBUF_LEN変更に相当する条件を、共通ヘッダーを書き換えず引数で切り替えています。
実際のセグメント・ACK・ウィンドウの挙動はtcpdumpで観察します。

## 5. D：受信を遅くする

送信サイズは基準の1000バイトに戻し、待ち時間だけ変えます。

```sh
./fserver.out test.dat 10000 1000
```

別ターミナル：

```sh
./fclient.out localhost 10000 received-slow.dat 1000 1000
cmp test.dat received-slow.dat
```

各read前に1ms待ちます。0μsの場合との時間、グッドプット、送受信回数を比較します。
必要なら10000μs（10ms）でも試せます。実際のスリープ時間は指定値より長くなることがあります。

## 6. ノード間でtcpdumpを使う

例：サーバーnode1、クライアントnode2。docker-compose.ymlのlan1-2を使う場合。

node1のターミナル1：

```sh
cd /home/ubuntu/share/Source/Lab/exp43
./fserver.out test.dat 10000 1000
```

node1のターミナル2（観察用。終了はCtrl+C）：

```sh
cd /home/ubuntu/share/Source/Lab/exp43
mkdir -p results
sudo tcpdump -i any -nn -s 0 -w results/slow-receive.pcap 'tcp port 10000'
```

node2：

```sh
cd /home/ubuntu/share/Source/Lab/exp43
./fclient.out 172.20.0.10 10000 received-node2.dat 1000 1000
cmp test.dat received-node2.dat
```

キャプチャを終了した後、node1で読む：

```sh
sudo tcpdump -nn -tttt -r results/slow-receive.pcap
```

Cでも条件ごとに別名のpcapを保存します。データ長（length）、ACK、広告ウィンドウ、
通信間隔などを比べます。表示されるウィンドウはTCPのスケーリングにも注意してください。
この構成ではshareフォルダはノード間で共有されるため、受信先は必ず別名にします。

## 7. localhostで条件をまとめて比較

```sh
make compare
```

4条件をそれぞれ3回実行し、受信ファイル・双方のログ・`results.csv` を
日時別のresultsフォルダへ保存します。全転送でSHA-256による内容一致を確認します。

1回ずつにする場合：

```sh
python3 compare_local.py --repeats 1
```

CSVの各行には送信サイズ、待ち時間、転送量、クライアントの時間・グッドプット、
双方の呼出回数、サーバーの時間、SHA-256が記録されます。
localhostの結果はノード間ネットワークの結果とは分けて扱ってください。
実際の実験ネットワークでの計測、tcpdumpによる観察と考察は別途行う必要があります。
