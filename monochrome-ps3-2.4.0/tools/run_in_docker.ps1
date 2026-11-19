# Compilador directo de Monochrome PS3 usando imagen oficial de Docker (ps3dev)
Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host " Compilacion PS3 sin instalar dependencias locales (Docker)" -ForegroundColor Cyan
Write-Host "==========================================================" -ForegroundColor Cyan

$currentDir = (Get-Item -Path ".").FullName

# Eliminar artefactos previos en la raiz para evitar empaquetar un .pkg viejo
Write-Host "[+] Eliminando artefactos previos (*.pkg, *.elf, *.self) en la raiz..." -ForegroundColor Yellow
Get-ChildItem -Path $currentDir -Filter "*.pkg" -File | Remove-Item -Force -ErrorAction SilentlyContinue
Get-ChildItem -Path $currentDir -Filter "*.elf" -File | Remove-Item -Force -ErrorAction SilentlyContinue
Get-ChildItem -Path $currentDir -Filter "*.self" -File | Remove-Item -Force -ErrorAction SilentlyContinue

Write-Host "[+] Ejecutando contenedor oficial ghcr.io/altps3/ps3dev:latest..." -ForegroundColor Green
docker run --rm -v "${currentDir}:/src" -w /src ghcr.io/altps3/ps3dev:latest sh -c "make clean && make pkg"

if ($LASTEXITCODE -ne 0) {
    Write-Host "[!] Error durante la compilacion en contenedor." -ForegroundColor Red
    exit 1
}

$pkgPath = Join-Path $currentDir "monochrome_ps3.pkg"

if (-not (Test-Path $pkgPath)) {
    Write-Host "[!] No se genero monochrome_ps3.pkg en la raiz." -ForegroundColor Red
    exit 1
}

$pkgFile = Get-Item $pkgPath
$now = Get-Date
$diff = ($now - $pkgFile.LastWriteTime).TotalSeconds

if ($diff -gt 60) {
    Write-Host "[!] monochrome_ps3.pkg no fue modificado recientemente (${diff}s). Posible .pkg desactualizado." -ForegroundColor Red
    exit 1
}

Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host " [OK] Build completado con exito" -ForegroundColor Green
Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host ("Ruta:   " + $pkgFile.FullName) -ForegroundColor Cyan
Write-Host ("Tamano: " + $pkgFile.Length + " bytes") -ForegroundColor Cyan
Write-Host ("Fecha:  " + $pkgFile.LastWriteTime) -ForegroundColor Cyan
Get-FileHash $pkgPath -Algorithm SHA256 | Format-List -Force
