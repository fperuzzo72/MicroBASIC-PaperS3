"""Patch em cpr-vcodex/src/network/FirmwareFlasher.cpp: protege o slot do
editor contra o autoupdate do próprio CPR-vCodex.

flashFromSdPath() sempre grava em esp_ota_get_next_update_partition() —
"o outro slot OTA" — sem saber que, num setup dual-boot, esse slot é onde
o MicroWriter (o editor) está. Sem isso, um autoupdate do CPR-vCodex
sobrescreveria o editor silenciosamente, ou deixaria o otadata apontando
pra algo que a troca dual-boot não reconhece mais.

destHoldsForeignApp() compara o app_desc embutido da partição-alvo com o
da partição rodando agora — se forem diferentes (é o editor, não uma
cópia antiga do próprio CPR-vCodex), recusa a atualização antes de apagar
qualquer byte.

O ponto de ancoragem (a chamada a esp_ota_get_next_update_partition() e o
comentário/null-check ao redor) é idêntico, byte a byte, entre CrossPoint,
CrossInk e CPR-vCodex — mesma ancestralidade CrossPoint. Por isso este
script e o equivalente em patches/crosspoint/ e patches/crossink/
aplicam exatamente o mesmo texto.
"""

path = "cpr-vcodex/src/network/FirmwareFlasher.cpp"
src = open(path).read()

# 1. Incluir esp_app_format.h (esp_app_desc_t) junto aos outros includes ESP-IDF.
old_includes = "#include <esp_ota_ops.h>\n#include <esp_partition.h>"
new_includes = "#include <esp_app_format.h>\n#include <esp_ota_ops.h>\n#include <esp_partition.h>"
assert old_includes in src, "esp_ota_ops.h/esp_partition.h includes not found"
src = src.replace(old_includes, new_includes, 1)

# 2. Função destHoldsForeignApp() — inserida logo antes de flashFromSdPath(),
#    fora de qualquer namespace anônimo (precisa ser chamável de fora deste
#    arquivo, para checagem antecipada na tela de confirmação de update).
guard_fn = '''
// True if `dest` currently holds a different app than the one running now —
// i.e. it's the dual-boot sibling (the MicroWriter editor), not a spare A/B
// slot for this same firmware. An unflashed/unreadable partition (no valid
// esp_app_desc_t) is treated as safe: there's no sibling to protect.
bool destHoldsForeignApp(const esp_partition_t* dest) {
  esp_app_desc_t myDesc;
  esp_app_desc_t destDesc;
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (!running) return false;
  if (esp_ota_get_partition_description(running, &myDesc) != ESP_OK) return false;
  if (esp_ota_get_partition_description(dest, &destDesc) != ESP_OK) return false;
  return strncmp(myDesc.project_name, destDesc.project_name, sizeof(myDesc.project_name)) != 0;
}

'''

anchor_fn = "Result flashFromSdPath(const char* sdPath, ProgressCb onProgress, void* ctx, bool alreadyValidated) {"
assert anchor_fn in src, "flashFromSdPath signature not found"
src = src.replace(anchor_fn, guard_fn + anchor_fn, 1)

# 3. A chamada do guard, logo após resolver `dest` — antes de qualquer
#    validação/gravação. Ancorado no comentário + null-check, que são
#    idênticos entre CrossPoint/CrossInk/CPR-vCodex.
old_dest = '''  const esp_partition_t* dest = esp_ota_get_next_update_partition(nullptr);
  if (!dest) {
    LOG_ERR("FLASH", "no next-update partition");
    return Result::NO_PARTITION;
  }
'''
new_dest = old_dest + '''
  if (destHoldsForeignApp(dest)) {
    LOG_ERR("FLASH", "next-update partition '%s' holds a different app (dual-boot sibling) — refusing to overwrite",
            dest->label);
    return Result::SIBLING_APP_PROTECTED;
  }
'''
assert old_dest in src, "flashFromSdPath's dest-resolution block not found"
src = src.replace(old_dest, new_dest, 1)

# 4. Novo valor no enum Result (declarado no .h).
open(path, "w").write(src)
print("FirmwareFlasher.cpp: destHoldsForeignApp + guard call OK")

# 5. Result::SIBLING_APP_PROTECTED no header, junto com o resultName() que
#    o converte pra string de log.
h_path = "cpr-vcodex/src/network/FirmwareFlasher.h"
h_src = open(h_path).read()

old_enum = "  OTADATA_FAIL,\n};"
new_enum = "  OTADATA_FAIL,\n  SIBLING_APP_PROTECTED,  // next-update partition holds a foreign app (dual-boot sibling)\n};"
assert old_enum in h_src, "Result enum closing not found in header"
h_src = h_src.replace(old_enum, new_enum, 1)

open(h_path, "w").write(h_src)
print("FirmwareFlasher.h: SIBLING_APP_PROTECTED added to Result enum")

cpp_path = path
cpp_src = open(cpp_path).read()
old_result_name = '    case Result::OTADATA_FAIL:\n      return "OTADATA_FAIL";'
new_result_name = old_result_name + '\n    case Result::SIBLING_APP_PROTECTED:\n      return "SIBLING_APP_PROTECTED";'
assert old_result_name in cpp_src, "resultName() OTADATA_FAIL case not found"
cpp_src = cpp_src.replace(old_result_name, new_result_name, 1)
open(cpp_path, "w").write(cpp_src)
print("FirmwareFlasher.cpp: resultName() case added")
