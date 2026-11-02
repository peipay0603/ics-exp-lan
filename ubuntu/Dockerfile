FROM ubuntu:18.04

RUN sed -i 's:^path-exclude=/usr/share/man:#path-exclude=/usr/share/man:' /etc/dpkg/dpkg.cfg.d/excludes

# "E: Failed to fetch (......) Hash Sum mismatch" のようなエラーが出たら以下のコメントアウトを外して make build などしてみると良い
# RUN printf "Acquire::http::Pipeline-Depth 0;\nAcquire::http::No-Cache true;\nAcquire::BrokenProxy true;" > /etc/apt/apt.conf.d/99fixbadproxy

RUN apt-get update \
  && apt-get install -y language-pack-ja-base language-pack-ja \
  && apt-get install -y sudo man manpages-posix less \
  && apt-get install -y build-essential vim emacs zip unzip git \
  && apt-get install -y python3.8 python3.8-venv python3-pip \
  && apt-get install -y iproute2 iputils-ping dnsutils netcat tcpdump nmap \
  && apt-get install -y iperf3 \
  && apt-get -y clean \
  && rm -rf /var/lib/apt/lists/*

RUN locale-gen ja_JP.UTF-8
ENV LANG ja_JP.UTF-8

# Setup root user
ENV PAGER less
RUN echo "root:lanexp" | chpasswd

# Setup ubuntu user
ARG UID
ARG GID
RUN groupadd -g $GID ubuntu
RUN useradd -m -u $UID -g $GID -s /bin/bash ubuntu
RUN echo "ubuntu:lanexp" | chpasswd
RUN gpasswd -a ubuntu sudo
USER ubuntu
ENV PAGER less
WORKDIR /home/ubuntu/share

