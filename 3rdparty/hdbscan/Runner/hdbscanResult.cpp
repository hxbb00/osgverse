#include "hdbscanResult.hpp"

hdbscanResult::hdbscanResult() {
	;
}
hdbscanResult::hdbscanResult(std::vector<int> pLables, std::vector<outlierScore> pOutlierScores,
                             std::vector<double> pmembershipProbabilities, bool pHsInfiniteStability) {
	labels = pLables;
	outliersScores = pOutlierScores;
	membershipProbabilities = pmembershipProbabilities;
	hasInfiniteStability = pHsInfiniteStability;
}
