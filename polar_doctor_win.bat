@echo off
REM Script de lancement pour Polar Doctor sur Windows
REM Ce script configure l'environnement GTK pour éviter les crashes

REM Force GTK à utiliser le backend GDK natif Windows
set GTK_USE_PORTAL=0
set GDK_BACKEND=win32

REM Lance Polar Doctor
polar_doctor.exe %*
