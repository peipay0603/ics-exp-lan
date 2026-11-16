# 実験6：経路を測って選ぶTCPファイル転送

授業資料の最終課題では、フルメッシュの知識と中継の利用、準備用プログラムの分離、
コマンドライン引数による経路指定が許されています。
4-1のファイル転送を元に、次の工夫を加えました。

1. 直接経路とnode3・node4・node5を経由する経路を、同じ1MiBの転送で測る。
2. 内容が一致した候補のうち、受信・保存にかかった時間が最短の経路を選ぶ。
3. 本番のファイルは選んだ経路で送る。read/writeのBUF_LENを64000にする。

中継は接続を受け付け、要求を上流へ渡し、その後は上流からreadしたデータを下流へwriteします。
圧縮やネットワーク設定変更はしていません。ファイルはTCPで実際に転送しています。
共有フォルダは送信元・保存先・内容比較のために使い、受信データの代わりにコピーしていません。

## ファイル

- fserver.c：4-1からのファイル送信処理。
- fclient.c：要求、受信、保存、時間とグッドプットの表示。
- relay.c：TCPの要求とファイルを中継。
- choose_route.sh：準備用。候補の実測、最短経路の選択、本番クライアントの起動。
- baseline-server.out・baseline-client.out：同じ基本転送処理をBUF_LEN=1000でビルドした比較用。

```sh
cd /home/ubuntu/share/Source/Lab/final
make
```

## 起動例（配布設定start1、node1→node2）

今回の実測データはshare/results/20261109-fullにあります。
Rは各ノードで共通のパスです。

node1で別ターミナルごとにサーバーを起動します。

```sh
R=/home/ubuntu/share/results/20261109-full
./fserver.out "$R/final.dat" 10070
```

```sh
R=/home/ubuntu/share/results/20261109-full
./fserver.out "$R/test.dat" 10071
```

中継ノードは2ポート分のrelayを別ターミナルで起動します。

| ノード | 上流IP | 起動するコマンド |
|---|---|---|
| node3 | 172.21.0.10 | ./relay.out 10070 172.21.0.10 10070 と ./relay.out 10071 172.21.0.10 10071 |
| node4 | 172.22.0.10 | ./relay.out 10070 172.22.0.10 10070 と ./relay.out 10071 172.22.0.10 10071 |
| node5 | 172.23.0.10 | ./relay.out 10070 172.23.0.10 10070 と ./relay.out 10071 172.23.0.10 10071 |

node2で候補を測り、本番転送します。

```sh
cd /home/ubuntu/share/Source/Lab/final
R=/home/ubuntu/share/results/20261109-full
bash choose_route.sh 10071 10070 "$R/test.dat" "$R/optimized-received.dat" "$R/routes" \
    172.20.0.10 172.24.0.30 172.25.0.40 172.26.0.50
cmp "$R/final.dat" "$R/optimized-received.dat"
```

候補は順に直接・node3経由・node4経由・node5経由です。
選択結果と各候補の秒数・グッドプットはroutes内のTSVとログに保存します。
候補の固定順で選ぶのではなく、測定結果で選びます。
この方法は複数経路の同時転送ではありません。

## 比較条件

- 基準：100MiB、node1→node2の直接経路、BUF_LEN=1000。
- 高速化版：同じ100MiB、実測で選んだ経路、BUF_LEN=64000。
- 100秒以上の本番サイズ確認：640MiBを選択経路で転送。

100MiB同士の結果から速度比を求めます。640MiBの結果と100MiBの時間を直接比較しません。
640MiBは100MB以上という条件に加え、高速化版でも100秒以上の転送を実際に確認するためです。
すべて同じstart1設定のままで行います。

基準の実行例：

node1：

```sh
./baseline-server.out /home/ubuntu/share/results/20261109-full/final.dat 10060
```

node2：

```sh
time -p ./baseline-client.out 172.20.0.10 10060 /home/ubuntu/share/results/20261109-full/baseline-received.dat
```

640MiBの確認では、node1のfinal-long.datをポート10072で配信し、
各中継も同じ上流IPで10072を中継します。クライアントはselected-host.txtに書かれたIPへ接続します。

## 説明するときのポイント

- 帯域の細い直接リンクを迂回し、使える中継経路を実測で選ぶ。
- BUF_LENを増やすとシステムコールの回数を減らせるが、それだけの改善量と経路変更の改善量は今回の比較では分離していない。
- 1MiBの試験転送は本番の性能を推定する方法であり、すべてのネットワーク・ファイルサイズで最良と保証するものではない。
- 準備のプローブ時間と本番クライアントの転送時間は別に記録する。準備を含む利用時間も説明できるようにする。
- 受信側で時間を測り、最後にcmpで内容を比較する。
- 学内でのTA立会い、班での担当確認、発表、提出は別の手続き。
