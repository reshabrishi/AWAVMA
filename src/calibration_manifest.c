#define _POSIX_C_SOURCE 200809L
#include "calibration_manifest.h"

#include <ctype.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MANIFEST_MAX_BYTES (1024U * 1024U)
#define MANIFEST_MAX_TOPOLOGIES 256U

typedef struct { const char *cursor; const char *end; } json_reader_t;
typedef struct {
    unsigned seen;
    unsigned schema_version;
    size_t record_count;
    bool production_authority;
    bool validations[6];
    char kind[64], mode[16], status[32], experiment[CALIBRATION_TEXT_MAX];
    char restore[16], metric[CALIBRATION_TEXT_MAX], basename[256], hash[65], version[CALIBRATION_TEXT_MAX];
    char topologies[MANIFEST_MAX_TOPOLOGIES][CALIBRATION_ID_MAX];
    size_t topology_count;
} authority_manifest_t;

static void reason_set(char reason[CALIBRATION_MANIFEST_REASON_MAX], const char *value)
{ if (reason != NULL) snprintf(reason, CALIBRATION_MANIFEST_REASON_MAX, "%s", value); }
static void whitespace(json_reader_t *r) { while (r->cursor < r->end && isspace((unsigned char)*r->cursor)) r->cursor++; }
static bool character(json_reader_t *r, char value) { whitespace(r); if (r->cursor >= r->end || *r->cursor != value) return false; r->cursor++; return true; }
static bool string_value(json_reader_t *r, char *out, size_t size)
{
    size_t used = 0;
    whitespace(r); if (r->cursor >= r->end || *r->cursor++ != '"') return false;
    while (r->cursor < r->end && *r->cursor != '"') {
        unsigned char c = (unsigned char)*r->cursor++;
        if (c < 0x20 || c == '\\' || used + 1 >= size) return false;
        out[used++] = (char)c;
    }
    if (r->cursor >= r->end || *r->cursor++ != '"') return false;
    out[used] = '\0'; return true;
}
static bool literal(json_reader_t *r, const char *text)
{ size_t n = strlen(text); whitespace(r); if ((size_t)(r->end-r->cursor) < n || memcmp(r->cursor,text,n)) return false; r->cursor += n; return true; }
static bool boolean(json_reader_t *r, bool *value)
{ if (literal(r,"true")) { *value=true; return true; } if (literal(r,"false")) { *value=false; return true; } return false; }
static bool unsigned_value(json_reader_t *r, size_t *value)
{
    size_t result=0; whitespace(r); if (r->cursor>=r->end || !isdigit((unsigned char)*r->cursor)) return false;
    do { unsigned digit=(unsigned)(*r->cursor++-'0'); if (result > (SIZE_MAX-digit)/10) return false; result=result*10+digit; } while (r->cursor<r->end && isdigit((unsigned char)*r->cursor));
    *value=result; return true;
}
static bool skip_value(json_reader_t *r, unsigned depth);
static bool skip_compound(json_reader_t *r, char open, char close, unsigned depth)
{
    char text[256];
    if (depth > 16 || !character(r,open)) return false;
    whitespace(r); if (r->cursor<r->end && *r->cursor==close) { r->cursor++; return true; }
    for (;;) {
        if (open=='{' && (!string_value(r,text,sizeof(text)) || !character(r,':'))) return false;
        if (!skip_value(r,depth+1)) return false;
        whitespace(r); if (r->cursor<r->end && *r->cursor==close) { r->cursor++; return true; }
        if (!character(r,',')) return false;
    }
}
static bool skip_value(json_reader_t *r, unsigned depth)
{
    char text[256]; whitespace(r); if (r->cursor>=r->end) return false;
    if (*r->cursor=='"') return string_value(r,text,sizeof(text));
    if (*r->cursor=='{') return skip_compound(r,'{','}',depth);
    if (*r->cursor=='[') return skip_compound(r,'[',']',depth);
    if (*r->cursor=='t') return literal(r,"true");
    if (*r->cursor=='f') return literal(r,"false");
    if (*r->cursor=='n') return literal(r,"null");
    if (*r->cursor=='-' || isdigit((unsigned char)*r->cursor)) { r->cursor++; while (r->cursor<r->end && strchr("0123456789.eE+-",*r->cursor)) r->cursor++; return true; }
    return false;
}
static bool topology_array(json_reader_t *r, authority_manifest_t *m)
{
    if (!character(r,'[')) return false;
    whitespace(r);
    if (r->cursor<r->end && *r->cursor==']') { r->cursor++; return true; }
    for (;;) { if (m->topology_count>=MANIFEST_MAX_TOPOLOGIES || !string_value(r,m->topologies[m->topology_count++],CALIBRATION_ID_MAX)) return false; whitespace(r); if (r->cursor<r->end && *r->cursor==']') { r->cursor++; return true; } if (!character(r,',')) return false; }
}
static int field_index(const char *key)
{
    static const char *fields[]={"schema_version","kind","mode","status","production_authority","collection_experiment_id","numa_balancing_restore_status","timing_valid","cost_migration_valid","placement_validation_valid","numa_balancing_transaction_valid","strict_validation_valid","overall_valid","timing_metric","calibration_artifact_basename","calibration_artifact_sha256","calibration_record_count","calibration_version","topology_fingerprints","raw_artifacts"};
    for (int i=0;i<20;i++) if (!strcmp(key,fields[i])) return i;
    return -1;
}
static bool parse_manifest(const char *text, size_t length, authority_manifest_t *m)
{
    json_reader_t r={text,text+length}; char key[96];
    if (!character(&r,'{')) return false;
    whitespace(&r);
    if (r.cursor<r.end && *r.cursor=='}') return false;
    for (;;) {
        int field; size_t number;
        if (!string_value(&r,key,sizeof(key)) || !character(&r,':')) return false;
        field=field_index(key); if (field>=0 && (m->seen&(1U<<field))) return false; if (field>=0) m->seen|=1U<<field;
        switch(field) {
        case 0: if(!unsigned_value(&r,&number)||number>0xffffffffU)return false; m->schema_version=(unsigned)number; break;
        case 1: if(!string_value(&r,m->kind,sizeof(m->kind)))return false; break; case 2: if(!string_value(&r,m->mode,sizeof(m->mode)))return false; break;
        case 3: if(!string_value(&r,m->status,sizeof(m->status)))return false; break; case 4: if(!boolean(&r,&m->production_authority))return false; break;
        case 5: if(!string_value(&r,m->experiment,sizeof(m->experiment)))return false; break; case 6: if(!string_value(&r,m->restore,sizeof(m->restore)))return false; break;
        case 7: case 8: case 9: case 10: case 11: case 12: if(!boolean(&r,&m->validations[field-7]))return false; break;
        case 13: if(!string_value(&r,m->metric,sizeof(m->metric)))return false; break;
        case 14: if(!string_value(&r,m->basename,sizeof(m->basename)))return false; break; case 15: if(!string_value(&r,m->hash,sizeof(m->hash)))return false; break;
        case 16: if(!unsigned_value(&r,&m->record_count))return false; break; case 17: if(!string_value(&r,m->version,sizeof(m->version)))return false; break;
        case 18: if(!topology_array(&r,m))return false; break; default: if(!skip_value(&r,0))return false;
        }
        whitespace(&r); if(r.cursor<r.end&&*r.cursor=='}'){r.cursor++;break;} if(!character(&r,','))return false;
    }
    whitespace(&r); return r.cursor==r.end && m->seen==((1U<<20)-1U);
}
static bool file_sha256(const char *path, char output[65])
{
    FILE *file=fopen(path,"rb"); EVP_MD_CTX *context=NULL; unsigned char buffer[8192],digest[EVP_MAX_MD_SIZE]; unsigned length=0; size_t got; bool ok=false;
    if(!file||(context=EVP_MD_CTX_new())==NULL||EVP_DigestInit_ex(context,EVP_sha256(),NULL)!=1) goto done;
    while((got=fread(buffer,1,sizeof(buffer),file))>0) if(EVP_DigestUpdate(context,buffer,got)!=1) goto done;
    if(ferror(file)||EVP_DigestFinal_ex(context,digest,&length)!=1||length!=32) goto done;
    for(unsigned i=0;i<length;i++) snprintf(output+i*2,3,"%02x",digest[i]);
    ok=true;
done: if(context)EVP_MD_CTX_free(context); if(file)fclose(file); return ok;
}
bool calibration_manifest_verify(const char *artifact, const char *manifest_path, const CalibrationSnapshot *snapshot, char reason[CALIBRATION_MANIFEST_REASON_MAX])
{
    FILE *file; long size; char *text=NULL,hash[65]; authority_manifest_t m={0}; const char *base; bool ok=false; size_t unique_topologies=0;
    reason_set(reason,"CALIBRATION_MANIFEST_MALFORMED"); if(!artifact||!manifest_path||!snapshot||(file=fopen(manifest_path,"rb"))==NULL)return false;
    if(fseek(file,0,SEEK_END)|| (size=ftell(file))<=0 || size>(long)MANIFEST_MAX_BYTES || fseek(file,0,SEEK_SET) || (text=malloc((size_t)size))==NULL || fread(text,1,(size_t)size,file)!=(size_t)size){fclose(file);free(text);return false;} fclose(file);
    if(!parse_manifest(text,(size_t)size,&m))goto done;
    if(m.schema_version!=4||strcmp(m.kind,"AWAVMA_P4_CALIBRATION_AUTHORITY")){reason_set(reason,"CALIBRATION_AUTHORITY_SCHEMA_INVALID");goto done;}
    if(strcmp(m.mode,"full")||!m.production_authority){reason_set(reason,"CALIBRATION_NOT_PRODUCTION_AUTHORITY");goto done;}
    if(strcmp(m.status,"VALIDATED_PRODUCTION")){reason_set(reason,"CALIBRATION_AUTHORITY_STATUS_INVALID");goto done;}
    if(strcmp(m.metric,"throughput_ops_sec")){reason_set(reason,"CALIBRATION_AUTHORITY_METRIC_PROVENANCE_INVALID");goto done;}
    for(size_t i=0;i<6;i++)if(!m.validations[i]){reason_set(reason,"CALIBRATION_AUTHORITY_VALIDATION_FAILED");goto done;}
    if(strcmp(m.restore,"RESTORED")){reason_set(reason,"CALIBRATION_AUTHORITY_RESTORE_FAILED");goto done;}
    base=strrchr(artifact,'/'); base=base?base+1:artifact;
    if(strcmp(base,m.basename)){reason_set(reason,"CALIBRATION_ARTIFACT_IDENTITY_MISMATCH");goto done;}
    if(strlen(m.hash)!=64||!file_sha256(artifact,hash)||strcmp(hash,m.hash)){reason_set(reason,"CALIBRATION_ARTIFACT_HASH_MISMATCH");goto done;}
    if(snapshot->count==0||m.record_count!=snapshot->count){reason_set(reason,"CALIBRATION_RECORD_COUNT_MISMATCH");goto done;}
    if(strcmp(m.version,"p4c-v2")){reason_set(reason,"CALIBRATION_VERSION_MISMATCH");goto done;}
    for(size_t j=0;j<m.topology_count;j++) for(size_t k=j+1;k<m.topology_count;k++)
        if(!strcmp(m.topologies[j],m.topologies[k])) goto topology_bad;
    for(size_t i=0;i<snapshot->count;i++){
        const CalibrationRecord *record=&snapshot->records[i]; bool topology=false;
        if(record->status!=CALIBRATION_VALIDATED_PRODUCTION||strcmp(record->calibration_version,"p4c-v2")||strcmp(record->collection_experiment_id,m.experiment)){reason_set(reason,"CALIBRATION_ARTIFACT_PROVENANCE_MISMATCH");goto done;}
        for(size_t j=0;j<m.topology_count;j++)if(!strcmp(record->compatibility.topology_fingerprint,m.topologies[j]))topology=true;
        if(!topology){reason_set(reason,"CALIBRATION_TOPOLOGY_IDENTITY_MISMATCH");goto done;}
        bool first=true; for(size_t k=0;k<i;k++) if(!strcmp(record->compatibility.topology_fingerprint,snapshot->records[k].compatibility.topology_fingerprint)) first=false;
        if(first) unique_topologies++;
    }
    if(m.topology_count!=unique_topologies) goto topology_bad;
    for(size_t j=0;j<m.topology_count;j++){bool topology=false; if(m.topologies[j][0]=='\0')goto topology_bad; for(size_t i=0;i<snapshot->count;i++)if(!strcmp(m.topologies[j],snapshot->records[i].compatibility.topology_fingerprint))topology=true; if(!topology)goto topology_bad;}
    ok=true; reason_set(reason,"CALIBRATION_MANIFEST_TRUSTED"); goto done;
topology_bad: reason_set(reason,"CALIBRATION_TOPOLOGY_IDENTITY_MISMATCH");
done: free(text); return ok;
}
bool calibration_manifest_verify_buffers(const char *artifact_bytes, size_t artifact_length,
                                         const char *manifest_bytes, size_t manifest_length,
                                         const char *artifact_basename,
                                         const CalibrationSnapshot *snapshot,
                                         char reason[CALIBRATION_MANIFEST_REASON_MAX])
{
    FILE *artifact=NULL, *manifest=NULL; char directory[]="/tmp/awavma-calibration-XXXXXX", artifact_path[4096]="", manifest_path[4096]=""; bool result=false;
    if (artifact_bytes == NULL || manifest_bytes == NULL || artifact_basename == NULL || strchr(artifact_basename,'/') != NULL ||
        artifact_length == 0 || manifest_length == 0 || mkdtemp(directory) == NULL ||
        snprintf(artifact_path,sizeof(artifact_path),"%s/%s",directory,artifact_basename)>=(int)sizeof(artifact_path) ||
        snprintf(manifest_path,sizeof(manifest_path),"%s/manifest.json",directory)>=(int)sizeof(manifest_path) ||
        (artifact=fopen(artifact_path,"wb"))==NULL || (manifest=fopen(manifest_path,"wb"))==NULL) { reason_set(reason,"CALIBRATION_BUFFER_INVALID"); goto done; }
    if (fwrite(artifact_bytes,1,artifact_length,artifact)!=artifact_length || fclose(artifact)!=0) { reason_set(reason,"CALIBRATION_BUFFER_INVALID"); goto done; }
    artifact=NULL;
    if (fwrite(manifest_bytes,1,manifest_length,manifest)!=manifest_length || fclose(manifest)!=0) { reason_set(reason,"CALIBRATION_BUFFER_INVALID"); goto done; }
    manifest=NULL; result=calibration_manifest_verify(artifact_path,manifest_path,snapshot,reason);
done:
    if (artifact) fclose(artifact);
    if (manifest) fclose(manifest);
    unlink(artifact_path);
    unlink(manifest_path);
    rmdir(directory);
    return result;
}
