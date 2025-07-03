# 📝 Editor de Texto Colaborativo Distribuído

Este projeto implementa um editor de texto colaborativo que permite a edição simultânea de um documento por múltiplos usuários. A arquitetura combina **paralelismo com OpenMP** (para operações concorrentes dentro de cada processo) e **comunicação distribuída com MPI** (para interação entre usuários/processos).

## 🔧 Funcionalidades principais

- ✅ Suporte a múltiplos usuários conectados simultaneamente
- 🔒 Bloqueio de linhas para evitar conflitos de edição
- 💬 Sistema de mensagens privadas entre usuários
- 🔁 Comunicação ponto-a-ponto (peer-to-peer) e coletiva (broadcast)
- 🧪 Geração automática de dados para testes
- 🕓 Histórico de alterações realizadas
- 🖥️ Interface simples via terminal

## 🧰 Tecnologias utilizadas

- Linguagem **C**
- Biblioteca **OpenMP** para paralelismo em memória compartilhada
- Biblioteca **MPI** (MPICH ou OpenMPI) para comunicação entre processos

## 📦 Requisitos

- Compilador **C/C++** com suporte a OpenMP
- Ambiente com **MPI** instalado (ex: MPICH ou OpenMPI)
- Terminal Linux ou WSL (para Windows)

## Para rodar e necessario o xterm:

```
sudo apt install xterm
````

### compilar o codigo com:

```
mpicc -fopenmp -o editor_colaborativo editor_colaborativo.c
```

### Para rodar basta executar:
```
./run_multi_xterm.sh
```
