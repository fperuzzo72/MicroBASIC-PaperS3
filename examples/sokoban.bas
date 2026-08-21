1 REM ------------------------------------------
2 REM FSP MICROBASIC - EMPURRA CAIXAS
3 REM Precisa de SCREEN 1 ou maior: usa 13 linhas,
4 REM e o SCREEN 0 so tem 10. Um movimento por
5 REM tecla: o painel so repinta quando voce
6 REM decide algo, entao a espera vira pausa.
7 REM Setas empurram, R recomeca, Q sai.
8 REM ------------------------------------------
10 DIM W$(96):DIM B$(96):DIM O$(96)
20 NL=12:L=1
30 GOSUB 500
40 GOSUB 600
100 REM ---- espera uma tecla ----
110 GET K
120 IF K=0 THEN GOTO 110
130 REM ENTER reinicia a fase. Sair e ESC (BACK no
131 REM aparelho), que o interpretador trata como break --
132 REM por isso nao aparece aqui como tecla.
140 IF K=10 THEN GOTO 480
150 D=0
160 IF K=28 THEN D=1
170 IF K=29 THEN D=-1
180 IF K=31 THEN D=12
190 IF K=30 THEN D=-12
200 IF D=0 THEN GOTO 110
210 N=P+D
220 IF N<1 OR N>96 THEN GOTO 110
230 IF W$(N,N)="#" THEN GOTO 110
240 IF B$(N,N)<>"$" THEN GOTO 300
250 N2=N+D
260 IF N2<1 OR N2>96 THEN GOTO 110
270 IF W$(N2,N2)="#" THEN GOTO 110
280 IF B$(N2,N2)="$" THEN GOTO 110
290 B$(N,N)=" ":B$(N2,N2)="$":I=N2:GOSUB 700
300 REM P precisa mudar ANTES de repintar a casa
301 REM velha: 700 desenha "@" em qualquer casa que
302 REM seja P, entao repintar com P ainda apontando
303 REM para ela redesenhava o jogador em vez de
304 REM apaga-lo, deixando rastro por onde andou.
305 OI=P:P=N
306 I=OI:GOSUB 700
310 I=P:GOSUB 700
320 MV=MV+1
330 GOSUB 800
340 REM ---- terminou o nivel? ----
350 F=0
360 FOR I=1 TO 96
370 IF B$(I,I)="$" AND W$(I,I)<>"." THEN F=1
380 NEXT
390 IF F=1 THEN GOTO 110
400 L=L+1
410 IF L>NL THEN GOTO 440
420 GOSUB 500:GOSUB 600
430 GOTO 110
440 LOCATE 1,13:PRINT "TODOS OS NIVEIS!                     ";
450 END
460 LOCATE 1,13:PRINT "PAROU no nivel ";L;".                  ";
470 END
480 REM ---- recomeca o nivel ----
485 FOR I=1 TO 96
486 B$(I,I)=O$(I,I)
487 NEXT
488 P=OP:MV=0
490 GOSUB 600
495 GOTO 110
500 REM ---- carrega o nivel corrente do DATA ----
505 FOR R=1 TO 8
510 READ A$
515 FOR C=1 TO 12
520 I=(R-1)*12+C
525 Z$=A$(C,C)
530 W$(I,I)=" "
535 IF Z$="#" THEN W$(I,I)="#"
540 IF Z$="." OR Z$="*" OR Z$="+" THEN W$(I,I)="."
545 B$(I,I)=" "
550 IF Z$="$" OR Z$="*" THEN B$(I,I)="$"
555 IF Z$="@" OR Z$="+" THEN P=I
560 NEXT
565 NEXT
570 FOR I=1 TO 96
575 O$(I,I)=B$(I,I)
580 NEXT
585 OP=P:MV=0
590 RETURN
600 REM ---- desenha o tabuleiro inteiro ----
610 CLS
620 FOR I=1 TO 96
630 GOSUB 700
640 NEXT
650 GOSUB 800
655 GOSUB 820
660 LOCATE 1,13:PRINT "setas empurram  ENTER recomeca  ESC sai";
670 RETURN
820 REM ---- legenda, a esquerda do tabuleiro ----
825 REM O tabuleiro ocupa as colunas 18 a 29, entao
826 REM sobra a faixa 2..15 e ela nao encosta nele.
830 LOCATE 2,4:PRINT "# parede";
835 LOCATE 2,5:PRINT "$ caixa";
840 LOCATE 2,6:PRINT ". destino";
845 LOCATE 2,7:PRINT "* caixa no";
850 LOCATE 2,8:PRINT "  destino";
855 LOCATE 2,9:PRINT "@ voce";
860 LOCATE 2,10:PRINT "+ voce no";
865 LOCATE 2,11:PRINT "  destino";
870 RETURN
700 REM ---- desenha a celula I ----
710 R=INT((I-1)/12)+1
720 C=I-(R-1)*12
730 Z$=W$(I,I)
740 IF B$(I,I)<>"$" THEN GOTO 770
750 IF W$(I,I)="." THEN Z$="*"
760 IF W$(I,I)<>"." THEN Z$="$"
765 GOTO 790
770 IF I<>P THEN GOTO 790
775 IF W$(I,I)="." THEN Z$="+"
780 IF W$(I,I)<>"." THEN Z$="@"
790 LOCATE 17+C,3+R:PRINT Z$;
795 RETURN
800 LOCATE 1,1:PRINT "NIVEL ";L;" de ";NL;"   MOVIMENTOS ";MV;"   ";
810 RETURN
990 DATA "############"
991 DATA "#          #"
992 DATA "#  #.#.#   #"
993 DATA "#   $ $    #"
994 DATA "#  # @ #   #"
995 DATA "#   $ $    #"
996 DATA "#  #.#.#   #"
997 DATA "############"
998 DATA "############"
999 DATA "#          #"
1000 DATA "#  ##  ##  #"
1001 DATA "#  #    #  #"
1002 DATA "#   $..$   #"
1003 DATA "#     @    #"
1004 DATA "#          #"
1005 DATA "############"
1006 DATA "############"
1007 DATA "#          #"
1008 DATA "#  #####   #"
1009 DATA "#  $ . #   #"
1010 DATA "#  . $ #   #"
1011 DATA "#   @  #   #"
1012 DATA "#  #####   #"
1013 DATA "############"
1014 DATA "############"
1015 DATA "#   #      #"
1016 DATA "# $ $ .. # #"
1017 DATA "#   #    # #"
1018 DATA "#  @#      #"
1019 DATA "#   ####   #"
1020 DATA "#          #"
1021 DATA "############"
1022 DATA "############"
1023 DATA "#          #"
1024 DATA "#  ####    #"
1025 DATA "#  #  # $  #"
1026 DATA "#  #. .  $ #"
1027 DATA "#  ####  @ #"
1028 DATA "#          #"
1029 DATA "############"
1030 DATA "############"
1031 DATA "#          #"
1032 DATA "# ####  ## #"
1033 DATA "# #  .  $  #"
1034 DATA "# # $ .@   #"
1035 DATA "# ######   #"
1036 DATA "#          #"
1037 DATA "############"
1038 DATA "############"
1039 DATA "#  ##      #"
1040 DATA "# $  $ ##  #"
1041 DATA "#  ..  #   #"
1042 DATA "# ## @ #   #"
1043 DATA "#      #   #"
1044 DATA "#  ##      #"
1045 DATA "############"
1046 DATA "############"
1047 DATA "#   ###    #"
1048 DATA "# . # $    #"
1049 DATA "#   #  @   #"
1050 DATA "# $ #  .   #"
1051 DATA "#   ####   #"
1052 DATA "#          #"
1053 DATA "############"
1054 DATA "############"
1055 DATA "#   #      #"
1056 DATA "# . # $    #"
1057 DATA "#   #   #  #"
1058 DATA "# . $   #  #"
1059 DATA "#   #  @#  #"
1060 DATA "#   #      #"
1061 DATA "############"
1062 DATA "############"
1063 DATA "#      #   #"
1064 DATA "# .$.$ #   #"
1065 DATA "#  ##  #   #"
1066 DATA "# $.$. @   #"
1067 DATA "#  ##  #   #"
1068 DATA "#      #   #"
1069 DATA "############"
1070 DATA "############"
1071 DATA "#   #      #"
1072 DATA "# $ # .    #"
1073 DATA "#   #  #   #"
1074 DATA "# $   .#   #"
1075 DATA "#   @  #   #"
1076 DATA "#      #   #"
1077 DATA "############"
1078 DATA "############"
1079 DATA "#   #      #"
1080 DATA "# . # $ .  #"
1081 DATA "#   #   #  #"
1082 DATA "# $ @   #  #"
1083 DATA "#   ### $  #"
1084 DATA "#  .       #"
1085 DATA "############"
