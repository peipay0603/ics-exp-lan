# LAN実験用Dockerネットワーク

## トポロジー図
実験教材のPDFを参考

コンテナは5台起動し、node1~5の名前がついています

## 必要環境
Docker, docker-compose

## 使い方
```
# Dockerイメージの作成
make build

# コンテナとネットワークを起動。ネットワークは4種類用意されており、start1~4のどれか1つを選んで実行する
make [start1|start2|start3|start4]

# 起動中のコンテナnode1~5にログイン
make login-node1
make login-node2
make login-node3
make login-node4
make login-node5

# コンテナを止める
make stop

# コンテナとネットワークを削除。ネットワークの種類を変える時(make start1~4をやり直したい時)に実行が必要
make clean

# 危険: Dockerのコンテナ・イメージ・ネットワーク・ボリュームを全て削除。自分が初めて実験環境を整える時だけ使う
make cleanall
```

すべてのコンテナは`/home/ubuntu/share`にホストの`./share`をマウントしているので、ファイルの共有にはここを使って下さい

## パスワード
ubuntuユーザー、rootユーザーのパスワードはともに`lanexp`です

## コマンド一覧
```
make build # Dockerイメージの作成
make [start1|start2|start3|start4] # コンテナとネットワークを起動。ネットワークは4種類用意されており、start1~4のどれか1つを選んで実行する
make stop # コンテナをすべて停止
make ps # 起動中のコンテナ一覧を表示
make log # コンテナのログを表示
make clean # 危険!: コンテナとネットワークをすべて削除
make cleanall # 危険!: Docker上のすべてのコンテナ・イメージ・ネットワーク・ボリュームを削除

# 起動中のコンテナnode1~5にログイン
make login-node1
make login-node2
make login-node3
make login-node4
make login-node5

# コンテナからのログアウト
exit # もしくは Ctrl+D
```

## 注意
- **`make clean`するとコンテナが削除され、コンテナ内のファイルも削除されるので要注意！**
  - 例外としてコンテナの`/home/ubuntu/share`の中は削除されずにホストの`./share`に残るので、心配ならここを中心に作業して下さい
- **`make cleanall`するとDocker上のほぼすべてのデータが削除されるので要注意！**
  - これは実験用PCを掃除するために使うコマンドです。指示のない限りは実行しないこと
