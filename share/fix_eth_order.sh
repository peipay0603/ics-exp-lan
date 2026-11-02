#!/bin/bash

# 1. 既存の eth* をすべて一旦 tmp* にリネームして衝突を防ぐ
# 修正点: 'cut -d@ -f1' を追加して @if... を削除する
interfaces=$(ip -o link show | awk -F': ' '{print $2}' | cut -d@ -f1 | grep '^eth')

i=0
for iface in $interfaces; do
    echo "Temporary renaming: $iface -> tmp$i"
    ip link set dev "$iface" name "tmp$i"
    i=$((i+1))
done

# 2. IPアドレス(サブネット)に基づいて、希望の eth* にリネームする関数
assign_eth() {
    SUBNET=$1
    NEW_NAME=$2
    
    # 指定したIPを持つインターフェース(tmp*)を探す
    # ここでも念のため cut でゴミを取る
    CURRENT_NAME=$(ip -o addr show | grep "$SUBNET" | awk '{print $2}')
    
    if [ -n "$CURRENT_NAME" ]; then
        echo "Renaming $CURRENT_NAME ($SUBNET) to $NEW_NAME"
        ip link set dev "$CURRENT_NAME" name "$NEW_NAME"
    else
        echo "Warning: Target IP $SUBNET not found."
    fi
}

# --- ここでルールを定義 ---

# node1 用のルール
if [ "$HOSTNAME" = "node1" ]; then
    assign_eth "172.20." "eth0" # lan1-2
    assign_eth "172.21." "eth1" # lan1-3
    assign_eth "172.22." "eth2" # lan1-4
    assign_eth "172.23." "eth3" # lan1-5
fi

# node2 用のルール
if [ "$HOSTNAME" = "node2" ]; then
    assign_eth "172.20." "eth0" # lan1-2
    assign_eth "172.24." "eth1" # lan2-3
    assign_eth "172.25." "eth2" # lan2-4
    assign_eth "172.26." "eth3" # lan2-5
fi

# node3 用のルール
if [ "$HOSTNAME" = "node3" ]; then
    assign_eth "172.21." "eth0" # lan1-3
    assign_eth "172.24." "eth1" # lan2-3
    assign_eth "172.27." "eth2" # lan3-4
    assign_eth "172.28." "eth3" # lan3-5
fi

# node4 用のルール
if [ "$HOSTNAME" = "node4" ]; then
    assign_eth "172.22." "eth0" # lan1-4
    assign_eth "172.25." "eth1" # lan2-4
    assign_eth "172.27." "eth2" # lan3-4
    assign_eth "172.29." "eth3" # lan4-5
fi

# node5 用のルール
if [ "$HOSTNAME" = "node5" ]; then
    assign_eth "172.23." "eth0" # lan1-5
    assign_eth "172.26." "eth1" # lan2-5
    assign_eth "172.28." "eth2" # lan3-5
    assign_eth "172.29." "eth3" # lan4-5
fi


