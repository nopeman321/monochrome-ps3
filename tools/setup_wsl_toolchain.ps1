# Script de preparacion de WSL para compilacion PS3
Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host " Instalador del Entorno de Compilacion PS3 (PSL1GHT v2)   " -ForegroundColor Cyan
Write-Host "==========================================================" -ForegroundColor Cyan

# 1. Verificar si WSL esta habilitado
$wslCheck = Get-Command wsl.exe -ErrorAction SilentlyContinue
if (-not $wslCheck) {
    Write-Host "[!] WSL no esta instalado. Ejecuta PowerShell como Administrador:" -ForegroundColor Red
    Write-Host "    wsl --install -d Ubuntu-24.04" -ForegroundColor Yellow
    exit 1
}

Write-Host "[+] Creando script de compilacion interna para Linux..." -ForegroundColor Green
$linuxScript = @"
#!/bin/bash
set -e
echo '[+] Actualizando paquetes e instalando dependencias de GCC/PPU/SPU...'
sudo apt-get update
sudo apt-get install -y build-essential autoconf automake bison flex texinfo \
    libncurses5-dev gcc-multilib g++-multilib git libelf-dev python3 \
    python3-pip zlib1g-dev libssl-dev pkg-config cmake ninja-build

export PS3DEV=/usr/local/ps3dev
export PSL1GHT=\$PS3DEV
export PATH=\$PATH:\$PS3DEV/bin:\$PS3DEV/ppu/bin:\$PS3DEV/spu/bin

if [ ! -d "\$PS3DEV" ]; then
    echo '[+] Instalando ps3toolchain...'
    sudo mkdir -p \$PS3DEV && sudo chown -R \$USER:\$USER \$PS3DEV
    git clone https://github.com/ps3dev/ps3toolchain.git /tmp/ps3toolchain
    cd /tmp/ps3toolchain && ./toolchain.sh
fi

echo '[+] Compilando Monochrome PS3...'
cd "\$(wslpath '$PSScriptRoot')"
make pkg
"@

Set-Content -Path "$PSScriptRoot\tools\build_in_wsl.sh" -Value $linuxScript -Encoding utf8
Write-Host "[+] Script generado en: tools/build_in_wsl.sh" -ForegroundColor Green
