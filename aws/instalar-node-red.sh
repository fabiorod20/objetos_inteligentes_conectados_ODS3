#!/bin/bash
# Instala o Node-RED + Dashboard 2.0 numa instância EC2 Ubuntu (AWS) e deixa rodando como serviço.
# Uso (dentro da EC2):  bash instalar-node-red.sh
# Depois: abrir a porta 1880 no Security Group e acessar http://<IP-PUBLICO>:1880
set -e

echo ">> Instalando Node.js 20..."
curl -fsSL https://deb.nodesource.com/setup_20.x | sudo -E bash -
sudo apt-get install -y nodejs

echo ">> Instalando Node-RED..."
sudo npm install -g --unsafe-perm node-red

echo ">> Instalando o Dashboard 2.0 (FlowFuse) na pasta do usuário..."
mkdir -p ~/.node-red
cd ~/.node-red
npm install @flowfuse/node-red-dashboard

echo ">> Criando o serviço para o Node-RED iniciar sozinho..."
sudo tee /etc/systemd/system/nodered.service > /dev/null <<EOF
[Unit]
Description=Node-RED
After=network.target

[Service]
User=$USER
WorkingDirectory=$HOME
ExecStart=/usr/bin/env node-red
Restart=on-failure

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable --now nodered

IP=$(curl -s http://checkip.amazonaws.com || echo "<IP-PUBLICO>")
echo ""
echo "Pronto. Editor:    http://$IP:1880"
echo "        Dashboard: http://$IP:1880/dashboard"
echo "Lembre de liberar a porta 1880 (TCP) no Security Group da instância."
