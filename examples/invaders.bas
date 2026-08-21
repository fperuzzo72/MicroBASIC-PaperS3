1 REM ------------------------------------------
2 REM FSP MICROBASIC - INVASORES
3 REM Precisa de SCREEN 2 (64 colunas, 20 linhas).
4 REM Setas movem, ESPACO atira, Q sai.
5 REM O tiro e instantaneo: a e-ink nao tem
6 REM quadros para um projetil subir devagar.
7 REM ------------------------------------------
10 PRINT "FSP INVASORES"
20 PRINT "Precisa de SCREEN 2 (64 colunas)."
30 INPUT "Ja esta em SCREEN 2 (S/N)";Q$
40 IF Q$="S" THEN GOTO 60
50 IF Q$<>"s" THEN PRINT "Digite  SCREEN 2  e depois RUN.":END
60 DIM V$(24):DIM R$(31)
70 CLS
80 FOR I=1 TO 24
90 V$(I,I)="M"
100 NEXT
110 OX=6:OY=3:D=1:PX=32:LX=0:SC=0:N=24
120 GOSUB 700
130 GOSUB 800
140 GOSUB 900
150 T=@T+250
160 IF @T<T THEN GOTO 160
170 T=@T+250
180 REM -- apaga o tiro do quadro anterior --
190 IF LX=0 THEN GOTO 240
200 FOR I=LR TO 15
210 LOCATE LX,I:PRINT " ";
220 NEXT
230 LX=0
240 GET K
250 IF K=113 THEN GOTO 640
260 IF K=29 THEN PX=PX-2
270 IF K=28 THEN PX=PX+2
280 IF PX<2 THEN PX=2
290 IF PX>63 THEN PX=63
300 IF K=32 THEN GOSUB 500
310 REM -- move a frota --
320 OX=OX+D
330 IF OX>=2 AND OX<=34 THEN GOTO 370
340 D=-D:OX=OX+D:OY=OY+1
350 GOSUB 750
360 IF OY+4>=15 THEN GOTO 620
370 GOSUB 700
380 GOSUB 800
390 IF N=0 THEN GOTO 600
400 GOTO 160
500 REM ---- TIRO: acha a coluna sob a nave ----
510 C=INT((PX-OX)/4)+1
520 IF C<1 THEN RETURN
530 IF C>8 THEN RETURN
540 IF ABS(PX-(OX+(C-1)*4))>1 THEN RETURN
550 FOR I=3 TO 1 STEP -1
560 P=(I-1)*8+C
570 IF V$(P,P)="M" THEN GOTO 580
575 NEXT
576 RETURN
580 V$(P,P)=" ":SC=SC+10:N=N-1
590 LX=OX+(C-1)*4:LR=OY+(I-1)*2
595 FOR J=LR TO 15
596 LOCATE LX,J:PRINT "!";
597 NEXT
598 RETURN
600 LOCATE 1,18:PRINT "LIMPOU A TELA! ";SC;" pontos.";
610 END
620 LOCATE 1,18:PRINT "INVADIRAM. ";SC;" pontos.";
630 END
640 LOCATE 1,18:PRINT "PAROU. ";SC;" pontos.";
650 END
700 REM ---- desenha a frota ----
710 FOR I=1 TO 3
720 R$="                               "
730 FOR C=1 TO 8
735 P=(I-1)*8+C
736 IF V$(P,P)="M" THEN R$(2+(C-1)*4,2+(C-1)*4)="M"
737 NEXT
738 LOCATE OX-1,OY+(I-1)*2
739 PRINT R$;
740 NEXT
745 RETURN
750 REM ---- limpa a faixa de jogo (a frota desceu) ----
755 REM 63 espacos a partir da coluna 1: a frota ocupa OX-1 ate
756 REM OX+29, e OX chega a 34, entao ela alcanca a coluna 63.
757 REM Limpar so ate a 61 deixava uma pilha presa na coluna 62.
760 FOR I=3 TO 14
770 LOCATE 1,I:PRINT "                                                               ";
780 NEXT
790 RETURN
800 REM ---- nave e placar ----
810 IF OP=PX THEN GOTO 830
820 LOCATE OP,16:PRINT " ";
830 LOCATE PX,16:PRINT "A";
840 OP=PX
850 LOCATE 1,1:PRINT "PONTOS ";SC;"   RESTAM ";N;"  ";
860 RETURN
900 LOCATE 1,20:PRINT "setas movem  espaco atira  Q sai";
910 RETURN
