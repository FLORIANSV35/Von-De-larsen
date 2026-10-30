// NOTE: written without access to a Mac; needs a first build + run on Apple Silicon.
#import <CoreML/CoreML.h>
#import <Foundation/Foundation.h>
#include <pthread.h>

#include <cstring>

#include "vc/CoreMlMaskProvider.h"

namespace vc {
namespace {

class CoreMlMask final : public AsyncMaskProvider {
public:
    CoreMlMask(MLModel* model, int nBins, int stateSize)
        : AsyncMaskProvider(nBins), nBins_(nBins), stateSize_(stateSize), model_(model)
    {
        NSError* err = nil;
        feat_ = [[MLMultiArray alloc] initWithShape:@[ @1, @(nBins) ] dataType:MLMultiArrayDataTypeFloat32 error:&err];
        if (stateSize_ > 0) {
            state_ = [[MLMultiArray alloc] initWithShape:@[ @1, @(stateSize_) ] dataType:MLMultiArrayDataTypeFloat32 error:&err];
            zeroState();
        }
        start();
    }
    ~CoreMlMask() override { stop(); }

protected:
    void onWorkerStart() override
    {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        // The first predictions compile/load the ANE program (hundreds of ms): do it before real audio arrives.
        std::vector<float> f(static_cast<size_t>(nBins_), -60.f), m(static_cast<size_t>(nBins_));
        for (int i = 0; i < 3; ++i) infer(f.data(), m.data());
        zeroState();
    }

    bool infer(const float* feat, float* mask) override
    {
        @autoreleasepool {
            NSError* err = nil;
            std::memcpy(feat_.dataPointer, feat, sizeof(float) * static_cast<size_t>(nBins_));
            NSMutableDictionary* d = [NSMutableDictionary dictionaryWithObject:[MLFeatureValue featureValueWithMultiArray:feat_]
                                                                        forKey:@"features"];
            if (state_) d[@"state_in"] = [MLFeatureValue featureValueWithMultiArray:state_];
            MLDictionaryFeatureProvider* in = [[MLDictionaryFeatureProvider alloc] initWithDictionary:d error:&err];
            if (!in) return false;
            id<MLFeatureProvider> out = [model_ predictionFromFeatures:in error:&err];
            if (!out) return false;
            MLMultiArray* m = [out featureValueForName:@"mask"].multiArrayValue;
            if (!m || !copyOut(m, mask, nBins_)) return false;
            if (state_) {
                MLMultiArray* s = [out featureValueForName:@"state_out"].multiArrayValue;
                if (!s || !copyOut(s, static_cast<float*>(state_.dataPointer), stateSize_)) return false;
            }
            return true;
        }
    }

private:
    void zeroState()
    {
        if (state_) std::memset(state_.dataPointer, 0, sizeof(float) * static_cast<size_t>(stateSize_));
    }

    // The ANE may return float16 and padded strides, so never memcpy blindly.
    static bool copyOut(MLMultiArray* a, float* dst, int n)
    {
        if (a.count < n) return false;
        const NSInteger stride = a.strides.lastObject.integerValue;
        if (a.dataType == MLMultiArrayDataTypeFloat32) {
            const float* p = static_cast<const float*>(a.dataPointer);
            for (int i = 0; i < n; ++i) dst[i] = p[i * stride];
            return true;
        }
        if (a.dataType == MLMultiArrayDataTypeFloat16) {
            const __fp16* p = static_cast<const __fp16*>(a.dataPointer);
            for (int i = 0; i < n; ++i) dst[i] = static_cast<float>(p[i * stride]);
            return true;
        }
        return false;
    }

    int nBins_, stateSize_;
    MLModel* model_;
    MLMultiArray* feat_ = nil;
    MLMultiArray* state_ = nil;
};

} // namespace

std::unique_ptr<AsyncMaskProvider> makeCoreMlMaskProvider(const std::string& path, int nBins, std::string& error)
{
    @autoreleasepool {
        NSError* err = nil;
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
        NSString* ext = url.pathExtension;
        if ([ext isEqualToString:@"mlpackage"] || [ext isEqualToString:@"mlmodel"]) {
            url = [MLModel compileModelAtURL:url error:&err];
            if (!url) { error = err.localizedDescription.UTF8String; return nullptr; }
        }
        MLModelConfiguration* cfg = [[MLModelConfiguration alloc] init];
        cfg.computeUnits = MLComputeUnitsCPUAndNeuralEngine;
        MLModel* model = [MLModel modelWithContentsOfURL:url configuration:cfg error:&err];
        if (!model) { error = err.localizedDescription.UTF8String; return nullptr; }

        MLFeatureDescription* sd = model.modelDescription.inputDescriptionsByName[@"state_in"];
        int stateSize = 0;
        if (sd && sd.multiArrayConstraint) stateSize = sd.multiArrayConstraint.shape.lastObject.intValue;
        if (!model.modelDescription.inputDescriptionsByName[@"features"] ||
            !model.modelDescription.outputDescriptionsByName[@"mask"]) {
            error = "model must have input 'features' and output 'mask'";
            return nullptr;
        }
        return std::make_unique<CoreMlMask>(model, nBins, stateSize);
    }
}

} // namespace vc
