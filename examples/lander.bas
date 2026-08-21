1 REM ------------------------------------------
2 REM FSP MICROBASIC - ALUNISSAGEM
3 REM Roda em qualquer SCREEN: so texto rolando.
4 REM Um turno por decisao, ideal para e-ink.
5 REM ------------------------------------------
10 PRINT "ALUNISSAGEM"
20 PRINT "Altitude 150m, combustivel 350."
30 PRINT "A cada turno diga quanto queimar, 0 a 30."
40 PRINT "Queima 10 anula a gravidade. Pouse abaixo de 5."
50 H=150:V=0:F=350:T=0
60 PRINT
70 PRINT "T";T;"  ALT ";INT(H);"  VEL ";INT(V);"  COMB ";INT(F)
80 IF H<=0 THEN GOTO 200
90 INPUT "QUEIMA";B
100 IF B<0 THEN B=0
110 IF B>30 THEN B=30
120 IF B>F THEN B=F
130 F=F-B
140 V=V+2-B*0.2
150 H=H-V
160 T=T+1
170 IF H<0 THEN H=0
180 GOTO 70
200 PRINT
210 IF V>=5 THEN GOTO 250
220 PRINT "POUSO PERFEITO! Velocidade ";INT(V)
230 PRINT "Sobraram ";INT(F);" de combustivel."
240 END
250 PRINT "CRASH. Chegou a ";INT(V);" m/s."
260 PRINT "Precisava de menos de 5."
270 END
