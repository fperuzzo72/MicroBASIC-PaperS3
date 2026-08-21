#include "sd_datetime.h"

#include "config.h"

#include <Arduino.h>
#include <SDCardManager.h>

#include <cstring>
#include <ctime>

namespace {

// Base de tempo desta sessao. 0 = nao temos data do leitor; usar a de
// compilacao. Resolvida uma vez em sdDateTimeSetup().
uint32_t baseEpoch = 0;
uint32_t baseMillis = 0;

// Data de compilacao, do __DATE__ ("Mmm dd yyyy"). Fallback quando o arquivo
// do leitor nao existe ou nao serve.
void compileDate(int* y, int* m, int* d) {
  static const char* MONTHS = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char* s = __DATE__;  // "Mmm dd yyyy"
  // Compara so os tres primeiros caracteres. A primeira versao passava a
  // string inteira para strstr(), que procurava "Aug 20 2026" dentro da
  // tabela de meses -- nunca casava, e todo arquivo saia em janeiro.
  *m = 1;
  for (int i = 0; i < 12; i++) {
    if (strncmp(s, MONTHS + i * 3, 3) == 0) { *m = i + 1; break; }
  }
  *d = atoi(s + 4);
  *y = atoi(s + 7);
}

// Procura lastKnownValidTimestamp em /.crosspoint/state.json.
//
// Varre byte a byte em vez de carregar o arquivo: o state.json do leitor tem
// varios KB e nao ha razao para gastar isso de RAM -- ainda mais neste
// firmware, onde bloco contiguo de heap ja foi problema (ver
// docs/DEVELOPMENT_LOG.md). Roda uma vez no arranque, entao a lentidao de ler
// caractere a caractere nao importa.
bool readReaderEpoch(uint32_t* out) {
  static const char KEY[] = "\"lastKnownValidTimestamp\"";
  static const size_t KL = sizeof(KEY) - 1;

  const char* PATH = "/.crosspoint/state.json";
  if (!SdMan.exists(PATH)) return false;   // caso normal, nao erro
  auto f = SdMan.open(PATH, O_RDONLY);
  if (!f.isOpen()) return false;

  size_t matched = 0;
  int c;
  while ((c = f.read()) >= 0) {
    if ((char)c == KEY[matched]) {
      if (++matched == KL) break;
    } else {
      matched = ((char)c == KEY[0]) ? 1 : 0;
    }
  }
  if (matched != KL) { f.close(); return false; }

  // Pula ':' e espacos, junta os digitos. Acumulado em uint32 de proposito:
  // o helper jsonGetInt() do projeto usa atoi(), e um epoch corrompido como
  // o 4154457600 que o leitor chegou a gravar estoura int e vira lixo com
  // sinal. Aqui ele chega inteiro e e rejeitado na faixa, abaixo.
  uint32_t v = 0;
  bool anyDigit = false;
  while ((c = f.read()) >= 0) {
    if (c == ':' || c == ' ') continue;
    if (c < '0' || c > '9') break;
    v = v * 10 + (uint32_t)(c - '0');
    anyDigit = true;
    if (v > 0xF0000000u) { f.close(); return false; }  // absurdo, desiste
  }
  f.close();
  if (!anyDigit || v == 0) return false;   // 0 e o valor inicial do leitor

  *out = v;
  return true;
}

// Faixa aceitavel para uma data vinda de fora.
//
// O piso de baixo e o mesmo do leitor (2024). O teto e o que **falta** nele, e
// e a razao de existir aqui: a validacao do CPR-vCodex so tem piso, entao um
// relogio corrompido de 2101 passa, e o std::max dele grava isso no cartao
// para sempre (ver docs/DEVELOPMENT_LOG.md). Se lermos aquele arquivo depois
// da corrupcao, herdariamos a mesma data absurda em todos os nossos arquivos.
//
// O teto e o ano de compilacao mais vinte: uma data decadas a frente do
// firmware que a esta lendo nao e uma data, e um defeito. Generoso o
// bastante para nunca recusar uso legitimo -- ninguem vai usar este binario
// em 2046.
//
// A FAT, por sua vez, so representa 1980..2107; a faixa abaixo cabe folgada
// dentro disso.
bool plausibleYear(int year) {
  int cy, cm, cd;
  compileDate(&cy, &cm, &cd);
  return year >= 2024 && year <= cy + 20;
}

// Faixa que a FAT consegue escrever. Usada para a data de compilacao, que nao
// vem de fora e so precisa ser representavel.
bool fatYearOk(int year) { return year >= 1980 && year <= 2107; }

void dateTimeCallback(uint16_t* date, uint16_t* time, uint8_t* ms10) {
  if (ms10) *ms10 = 0;

  if (baseEpoch != 0) {
    // Avanca com o tempo de sessao. Nao mexe no relogio de sistema de
    // proposito: a referencia dele vive na memoria RTC, cujo layout difere
    // entre binarios e cuja perturbacao foi a origem da data de 2101 no
    // leitor (ver docs/DEVELOPMENT_LOG.md). Aqui basta somar.
    const time_t t = (time_t)(baseEpoch + (millis() - baseMillis) / 1000UL);
    struct tm g;
    gmtime_r(&t, &g);
    const int y = g.tm_year + 1900;
    if (plausibleYear(y)) {
      *date = FS_DATE(y, g.tm_mon + 1, g.tm_mday);
      *time = FS_TIME(g.tm_hour, g.tm_min, g.tm_sec);
      return;
    }
  }

  int y, m, d;
  compileDate(&y, &m, &d);
  if (!fatYearOk(y)) { y = 1980; m = 1; d = 1; }
  *date = FS_DATE(y, m, d);
  *time = FS_TIME(0, 0, 0);
}

}  // namespace

void sdDateTimeSetup() {
  uint32_t epoch = 0;
  if (readReaderEpoch(&epoch)) {
    struct tm g;
    const time_t t = (time_t)epoch;
    gmtime_r(&t, &g);
    if (plausibleYear(g.tm_year + 1900)) {
      baseEpoch = epoch;
      baseMillis = millis();
      DBG_PRINTF("[SDDT] data do leitor: %04d-%02d-%02d\n",
                 g.tm_year + 1900, g.tm_mon + 1, g.tm_mday);
    }
  }
  if (baseEpoch == 0) DBG_PRINTLN("[SDDT] sem data do leitor, usando a de compilacao");
  FsDateTime::setCallback(dateTimeCallback);
}
