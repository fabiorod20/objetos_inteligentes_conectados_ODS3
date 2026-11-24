#!/bin/bash
# Instala o Node-RED numa instância EC2 Ubuntu (AWS), seguindo o manual da aula 2:
# atualiza o sistema -> nvm -> Node.js -> Node-RED -> pm2 (inicia sozinho no boot)
# e acrescenta o Dashboard 2.0 usado pelo painel do projeto.
#
# Uso (no terminal da EC2, usuário ubuntu):
#   curl -fsSLO https://raw.githubusercontent.com/fabiorod20/objetos_inteligentes_conectados_ODS3/main/aws/instalar-node-red.sh
#   bash instalar-node-red.sh
# Depois: liberar a porta 1880 (TCP) no Security Group e abrir http://<IP-PUBLICO>:1880

echo ">> 1/6 Atualizando o sistema..."
sudo apt-get update -y && sudo DEBIAN_FRONTEND=noninteractive apt-get upgrade -y

echo ">> 2/6 Instalando o nvm (gerenciador de versões do Node.js)..."
curl -fsSL https://raw.githubusercontent.com/nvm-sh/nvm/v0.40.3/install.sh | bash
export NVM_DIR="$HOME/.nvm"
. "$NVM_DIR/nvm.sh"

echo ">> 3/6 Instalando o Node.js (versão LTS)..."
nvm install --lts
node -v && npm -v

echo ">> 4/6 Instalando o Node-RED..."
npm install -g node-red

echo ">> 5/6 Instalando o Dashboard 2.0..."
mkdir -p ~/.node-red && cd ~/.node-red
npm install @flowfuse/node-red-dashboard
cd ~

echo ">> 6/6 Configurando o pm2 para o Node-RED iniciar sozinho..."
npm install -g pm2
pm2 start "$(command -v node-red)" --name node-red
sudo env PATH="$PATH" "$(command -v pm2)" startup systemd -u "$USER" --hp "$HOME"
pm2 save

if ! command -v node-red >/dev/null; then
  echo "ERRO: o Node-RED nao foi instalado. Rode o script de novo e veja a primeira mensagem de erro."
  exit 1
fi

sleep 5
ss -tpln | grep 1880 || echo "(o Node-RED ainda está subindo; confira com: pm2 status)"

IP=$(curl -s http://checkip.amazonaws.com || echo "<IP-PUBLICO>")
echo ""
echo "Pronto. Editor:    http://$IP:1880"
echo "        Dashboard: http://$IP:1880/dashboard"
echo "Lembre de liberar a porta 1880 (TCP) no Security Group da instância."
