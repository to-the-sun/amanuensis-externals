#include <stdio.h>
#include "cumulative_transience.h"

int main() {
    printf("--- STRUCT SIZES AND OFFSETS ---\n");
    printf("sizeof(PeakResult): %zu\n", sizeof(PeakResult));
    printf("sizeof(AnalyzerMetrics): %zu\n", sizeof(AnalyzerMetrics));
    printf("sizeof(PeakResultList): %zu\n", sizeof(PeakResultList));
    printf("sizeof(ChunkAnalysisResult): %zu\n", sizeof(ChunkAnalysisResult));

    ChunkAnalysisResult res;
    printf("Offset of metrics in ChunkAnalysisResult: %zu\n", (char*)&res.metrics - (char*)&res);
    printf("Offset of last_flux in ChunkAnalysisResult: %zu\n", (char*)&res.last_flux - (char*)&res);

    PeakResult pr;
    printf("Offset of detected_peak_val in PeakResult: %zu\n", (char*)&res.peak_list.peaks[0].detected_peak_val - (char*)&res.peak_list.peaks[0]);
    printf("Offset of qualifiers in PeakResult: %zu\n", (char*)&res.peak_list.peaks[0].qualifiers - (char*)&res.peak_list.peaks[0]);

    printf("\nCONSTANTS:\n");
    printf("MAX_PEAKS_PER_CHUNK: %d\n", MAX_PEAKS_PER_CHUNK);
    printf("MAX_QUALIFIERS: %d\n", MAX_QUALIFIERS);
    printf("BUFFER_LEN: %d\n", BUFFER_LEN);

    return 0;
}
