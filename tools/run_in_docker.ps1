# Compilador directo de Monochrome PS3 usando imagen oficial de Docker (ps3dev)
Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host " Compilacion PS3 sin instalar dependencias locales (Docker)" -ForegroundColor Cyan
Write-Host "==========================================================" -ForegroundColor Cyan

$currentDir = (Get-Item -Path ".").FullName

Write-Host "[+] Ejecutando contenedor oficial ghcr.io/ps3dev/ps3dev:latest..." -ForegroundColor Green
docker run --rm -v "${currentDir}:/src" -w /src ghcr.io/ps3dev/ps3dev:latest make pkg

if ($LASTEXITCODE -eq 0) {
    Write-Host "[OK] MCRM00001.pkg generado con exito en el directorio raiz!" -ForegroundColor Green
} else {
    Write-Host "[!] Error durante la compilacion en contenedor." -ForegroundColor Red
}
