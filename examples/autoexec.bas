1 REM ------------------------------------------
2 REM FSP MICROBASIC - LANCADOR (menu.bas)
3 REM Rode com  LOAD "menu.bas"  e  RUN.
4 REM
5 REM Gravado com o nome autoexec.bas ele roda
6 REM sozinho no arranque -- o interpretador
7 REM procura esse nome. Segurar BACK no boot
8 REM pula o autoexec, se algo der errado.
9 REM
10 REM Setas escolhem, ENTER confirma. Funciona
11 REM com o teclado BLE e com o d-pad do X4.
12 REM ------------------------------------------
20 DIM N$(96):DIM F$(16)
30 NP=6:SE=1
40 FOR I=1 TO NP
50 READ A$
60 N$((I-1)*16+1,I*16)=A$
70 NEXT
80 CLS
90 LOCATE 1,1:PRINT "FSP MicroBASIC v0.3 for XTeink X4";
100 LOCATE 1,3:PRINT "Escolha e tecle ENTER:";
110 GOSUB 500
120 GET K
130 IF K=0 THEN GOTO 120
140 IF K=30 THEN SE=SE-1
150 IF K=31 THEN SE=SE+1
160 IF SE<1 THEN SE=NP
170 IF SE>NP THEN SE=1
180 IF K=10 THEN GOTO 200
190 GOSUB 500
195 GOTO 120
200 REM ---- escolhido ----
210 IF SE=1 THEN GOTO 300
220 GOSUB 600
230 LOCATE 1,13:PRINT "Carregando ";F$;"...";
240 LOAD F$
250 END
300 LOCATE 1,13:PRINT "Pronto. Digite comandos BASIC.";
310 END
500 REM ---- desenha o menu ----
510 FOR I=1 TO NP
520 LOCATE 3,4+I
530 IF I=SE THEN PRINT ">";
540 IF I<>SE THEN PRINT " ";
550 PRINT N$((I-1)*16+1,I*16);
560 NEXT
570 RETURN
600 REM ---- nome do arquivo, sem os espacos a direita ----
610 S=(SE-1)*16+1
620 L=0
630 FOR I=1 TO 16
640 IF N$(S+I-1,S+I-1)<>" " AND L=I-1 THEN L=I
650 NEXT
660 F$=N$(S,S+L-1)
670 RETURN
900 DATA "SCREEN EDITOR   "
901 DATA "sokoban.bas     "
902 DATA "pacman.bas      "
903 DATA "invaders.bas    "
904 DATA "lander.bas      "
905 DATA "forca.bas       "
