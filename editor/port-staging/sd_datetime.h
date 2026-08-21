#pragma once

// Data e hora nos arquivos gravados no cartao SD.
//
// O SdFat so escreve os campos de data da entrada de diretorio se houver um
// callback registrado:
//
//     if (FsDateTime::callback) {
//       FsDateTime::callback(&date, &time, &ms10);
//       setLe16(dir->modifyDate, date);  ...
//     }
//
// Sem callback os campos ficam zerados, e a FAT le zero como 1980-00-00 --
// que e como os arquivos deste firmware apareciam ao abrir o cartao num PC.
//
// De onde vem a data, e por que dai
// ---------------------------------
// Este aparelho nao tem relogio: no X4 o DS3231 nao existe (o proprio
// CPR-vCodex desiste em HalClock::begin() se o hardware nao for um X3), e o
// relogio de sistema volta com lixo depois de qualquer troca de particao.
// Nao ha, portanto, hora verdadeira a oferecer por conta propria.
//
// O que existe e a data que o **leitor** ja mantem: ele guarda o ultimo
// instante valido que conheceu em /.crosspoint/state.json, no mesmo cartao,
// no campo lastKnownValidTimestamp. Ler dali nao e inventar data -- e usar um
// dado do proprio dispositivo, gravado por quem tinha como saber.
//
// IMPORTANTE, e o motivo deste paragrafo existir: isto foi verificado
// **somente contra o CPR-vCodex**. Os outros firmwares leitores da familia
// (CrossPoint, CrossInk) nao foram testados e provavelmente nao mantem nada
// parecido. Quem portar isto para outro contexto tem de conferir de novo, nao
// presumir. O codigo abaixo trata a ausencia do arquivo como caso normal
// justamente por isso.
//
// Quando o arquivo nao ajuda
// --------------------------
// Cai para a data de compilacao do firmware. Nao e a data real, mas e
// plausivel, ordenavel e nunca 1980. Os casos que caem aqui, todos legitimos:
//
//   - cartao novo ou formatado;
//   - MicroBASIC usado sozinho, sem o leitor nunca ter rodado no cartao;
//   - leitor presente mas nunca sincronizado (o campo vale 0);
//   - JSON malformado, campo ausente, ou esquema diferente numa versao futura;
//   - valor fora da faixa que a FAT representa (1980..2107).
//
// Em nenhum deles ha erro na tela nem recusa em gravar: uma data de arquivo e
// informacao acessoria, e falhar por causa dela seria pior que nao te-la.
//
// Lido uma vez so, no arranque. Nao ha por que reabrir o arquivo do leitor a
// cada arquivo que gravamos -- ele nao muda enquanto este firmware roda.

// Registra o callback do SdFat. Chamar uma vez, depois de SdMan.begin().
void sdDateTimeSetup();
