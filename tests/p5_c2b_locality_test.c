#include "p5_c2b_locality.h"
#include <stdio.h>
#include <string.h>
static int failures;
#define CHECK(name, condition) do { bool ok=(condition); printf("P5C2B_%s: %s\n",name,ok?"PASS":"FAIL"); failures+=!ok; } while (0)
int main(void) {
 P5C2BCell cells[6]; double rate=0.0; const double values[] = {1, 2, 3, 4, 100}; P5C2BStatistics stats; benchmark_placement_evidence_t placement;
 char placement_row[] = "1,remote,0,2,2,21,64,64,64,0,64,0,0,1.0,2,PASS,\"verified, remote\",true\n";
 CHECK("MATRIX_SIZE",p5_c2b_matrix(cells,6)==6);
 CHECK("PATTERNS",!strcmp(cells[0].pattern,"cold")&&!strcmp(cells[2].pattern,"moderate")&&!strcmp(cells[4].pattern,"hot"));
 CHECK("PLACEMENTS",!strcmp(cells[0].placement,"LOCAL")&&!strcmp(cells[1].placement,"REMOTE"));
 CHECK("CAPACITY",p5_c2b_matrix(cells,5)==0&&p5_c2b_matrix(NULL,6)==0);
 CHECK("RATE",p5_c2b_rate(250,10,&rate)&&rate==25.0);
 CHECK("ZERO_INTERVAL",!p5_c2b_rate(1,0,&rate));
 CHECK("ID",p5_c2b_identifier_valid("cloudlab_2026-10-10")&&!p5_c2b_identifier_valid("../escape")&&!p5_c2b_identifier_valid(""));
 CHECK("METRIC",!strcmp(P5_C2B_METRIC,"registered_memory_load_rate")&&!strcmp(P5_C2B_METRIC_UNIT,"ops/ms"));
 CHECK("WINDOW_K",P5_C2B_WINDOW_INTERVALS==5&&P5_C2B_EXPECTED_UNITS==14&&P5_C2B_MINIMUM_UNITS==12);
 CHECK("STATISTICS",p5_c2b_statistics(values,5,&stats)&&stats.minimum==1&&stats.maximum==100&&stats.mean==22&&stats.median==3);
 CHECK("LINEAR_PERCENTILES",stats.p10==1.4&&stats.p90>61.59&&stats.p90<61.61);
 CHECK("PLACEMENT_PARSE",p5_c2b_parse_placement_row(placement_row,&placement)&&placement.local_node==0&&placement.remote_node==2&&placement.requested_memory_node==2&&placement.numa_distance==21&&placement.remote_pages==64&&placement.memory_policy_restored);
 CHECK("PLACEMENT_RFC_TEXT",!strcmp(placement.verification_reason,"verified, remote"));
 return failures!=0;
}
