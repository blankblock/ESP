#include "ofApp.h"

#include <algorithm>

#include "user.h"

// If the feature output dimension is larger than 32, making the visualization a
// single output will be more visual.
const uint32_t kTooManyFeaturesThreshold = 32;
static const char* kInstruction =
        "Press capital P/T/A to change tabs.\n"
        "Press `s/p` to start/pause, 1-9 to record samples, "
        "`l` to load training data, and `t` to train a model.";

class Palette {
  public:
    vector<ofColor> generate(uint32_t n) {
        // TODO(benzh) fill instead of re-generate.
        if (n > size) {
            size = n;
            do_generate(size);
        }

        std::vector<ofColor> sliced(colors.begin(), colors.begin() + n);
        return sliced;
    }

    Palette() : size(256) {
        do_generate(size);
    }
  private:
    void do_generate(uint32_t n) {
        uint32_t numDimensions = n;
        // Code snippet from ofxGrtTimeseriesPlot.cpp

        colors.resize(n);
        // Setup the default colors
        if( numDimensions >= 1 ) colors[0] = ofColor(255,0,0); //red
        if( numDimensions >= 2 ) colors[1] = ofColor(0,255,0); //green
        if( numDimensions >= 3 ) colors[2] = ofColor(0,0,255); //blue
        //Randomize the remaining colors
        for(unsigned int n=3; n<numDimensions; n++){
            colors[n][0] = ofRandom(50,255);
            colors[n][1] = ofRandom(50,255);
            colors[n][2] = ofRandom(50,255);
        }
    }

    uint32_t size;
    std::vector<ofColor> colors;
};

void ofApp::useStream(IStream &stream) {
    istream_ = &stream;
}

void ofApp::usePipeline(GRT::GestureRecognitionPipeline &pipeline) {
    pipeline_ = &pipeline;
}

void ofApp::useOStream(OStream &stream) {
    ostream_ = &stream;
}

ofApp::ofApp() : fragment_(TRAINING),
                 num_pipeline_stages_(0),
                 ostream_(NULL),
                 should_save_training_data_(false) {
}

//--------------------------------------------------------------
void ofApp::setup() {

    is_recording_ = false;

    // setup() is a user-defined function.
    ::setup();

    if (ostream_ != NULL) {
        ostream_->setStreamSize(10000000);
        ostream_->start();
    }

    istream_->onDataReadyEvent(this, &ofApp::onDataIn);

    plot_inputs_.setup(kBufferSize_, istream_->getNumOutputDimensions(), "Input");
    plot_inputs_.setDrawGrid(true);
    plot_inputs_.setDrawInfoText(true);

    plot_testdata_.setup(kBufferSize_, istream_->getNumOutputDimensions(), "Test Data");
    plot_testdata_.setDrawGrid(true);
    plot_testdata_.setDrawInfoText(true);

    Palette color_palette;

    // Parse the user supplied pipeline and extract information:
    //  o num_pipeline_stages_

    // 1. Parse pre-processing.
    uint32_t num_pre_processing = pipeline_->getNumPreProcessingModules();
    num_pipeline_stages_ += num_pre_processing;
    for (int i = 0; i < num_pre_processing; i++) {
        PreProcessing* pp = pipeline_->getPreProcessingModule(i);
        uint32_t dim = pp->getNumOutputDimensions();
        ofxGrtTimeseriesPlot plot;
        plot.setup(kBufferSize_, dim, "PreProcessing Stage " + std::to_string(i));
        plot.setDrawGrid(true);
        plot.setDrawInfoText(true);
        plot.setColorPalette(color_palette.generate(dim));
        plot_pre_processed_.push_back(plot);
    }

    // 2. Parse pre-processing.
    uint32_t num_feature_modules = pipeline_->getNumFeatureExtractionModules();
    num_pipeline_stages_ += num_feature_modules;
    uint32_t num_final_features = 0;
    for (int i = 0; i < num_feature_modules; i++) {
        vector<ofxGrtTimeseriesPlot> feature_at_stage_i;

        FeatureExtraction* fe = pipeline_->getFeatureExtractionModule(i);
        uint32_t feature_dim = fe->getNumOutputDimensions();
        if (feature_dim < kTooManyFeaturesThreshold) {
            for (int i = 0; i < feature_dim; i++) {
                ofxGrtTimeseriesPlot plot;
                plot.setup(kBufferSize_, 1, "Feature " + std::to_string(i));
                plot.setDrawInfoText(true);
                plot.setColorPalette(color_palette.generate(feature_dim));
                feature_at_stage_i.push_back(plot);
            }
        } else {
            // We will have only one here.
            ofxGrtTimeseriesPlot plot;
            plot.setup(feature_dim, 1, "Feature");
            plot.setDrawGrid(true);
            plot.setDrawInfoText(true);
            plot.setColorPalette(color_palette.generate(feature_dim));
            feature_at_stage_i.push_back(plot);
        }
        num_final_features = feature_dim;

        plot_features_.push_back(feature_at_stage_i);
    }

    for (uint32_t i = 0; i < num_final_features; i++) {
        sample_feature_ranges_.push_back(make_pair(0, 0));
    }

    for (uint32_t i = 0; i < kNumMaxLabels_; i++) {
        uint32_t label_dim = istream_->getNumOutputDimensions();
        Plotter plot;
        plot.setup(label_dim, "Label" + std::to_string(i + 1));
        plot.setColorPalette(color_palette.generate(label_dim));
        plot_samples_.push_back(plot);


        vector<Plotter> feature_plots;
        if (num_final_features < kTooManyFeaturesThreshold) {
            // For this label, `num_final_features` vertically stacked plots
            for (int j = 0; j < num_final_features; j++) {
                Plotter plot;
                plot.setup(1, "Feature " + std::to_string(j + 1));
                plot.setColorPalette(color_palette.generate(label_dim));
                feature_plots.push_back(plot);
            }
        } else {
            // The case of many features (like FFT), draw a single plot.
            Plotter plot;
            plot.setup(1, "Feature");
            plot.setColorPalette(color_palette.generate(label_dim));
            feature_plots.push_back(plot);
        }
        plot_sample_features_.push_back(feature_plots);


        plot_sample_indices_.push_back(-1);
        plot_sample_button_locations_.push_back(pair<ofRectangle, ofRectangle>(ofRectangle(), ofRectangle()));

        TrainingSampleGuiListener *listener = new TrainingSampleGuiListener(this, i);
        ofxPanel *gui = new ofxPanel();
        gui->setup("Edit");
        gui->setSize(80, 0);

        ofxButton *rename_button = new ofxButton();
        gui->add(rename_button->setup("rename", 80, 16));
        rename_button->addListener(
            listener, &TrainingSampleGuiListener::renameButtonPressed);

        ofxButton *delete_button = new ofxButton();
        gui->add(delete_button->setup("delete", 80, 16));
        delete_button->addListener(
            listener, &TrainingSampleGuiListener::deleteButtonPressed);


        ofxButton *trim_button = new ofxButton();
        gui->add(trim_button->setup("trim", 80, 16));
        trim_button->addListener(
            listener, &TrainingSampleGuiListener::trimButtonPressed);

        ofxButton *relable_button = new ofxButton();
        gui->add(relable_button->setup("re-label", 80, 16));
        relable_button->addListener(
            listener, &TrainingSampleGuiListener::relabelButtonPressed);

        training_sample_guis_.push_back(gui);
    }

    for (uint32_t i = 0; i < plot_samples_.size(); i++) {
        plot_samples_[i].onRangeSelected(this, &ofApp::onPlotRangeSelected,
                                         reinterpret_cast<void*>(i + 1));
    }

    training_data_.setNumDimensions(istream_->getNumOutputDimensions());
//    training_data_.setDatasetName("Audio");
//    training_data_.setInfoText("This data contains audio data");
    predicted_label_ = 0;

    gui_.setup("", "", ofGetWidth() - 200, 0);
    gui_hide_ = true;
    gui_.add(save_pipeline_button_.setup("Save Pipeline", 200, 30));
    gui_.add(load_pipeline_button_.setup("Load Pipeline", 200, 30));
    gui_.add(save_training_data_button_.setup("Save Training Data", 200, 30));
    gui_.add(load_training_data_button_.setup("Load Training Data", 200, 30));
    save_pipeline_button_.addListener(this, &ofApp::savePipeline);
    load_pipeline_button_.addListener(this, &ofApp::loadPipeline);
    save_training_data_button_.addListener(this, &ofApp::saveTrainingData);
    load_training_data_button_.addListener(this, &ofApp::loadTrainingData);

    ofBackground(54, 54, 54);

    // After everything is setup, start streaming.
    istream_->start();
}

void ofApp::onPlotRangeSelected(Plotter::CallbackArgs arg) {
    uint32_t sample_index = reinterpret_cast<uint64_t>(arg.data) - 1;
    uint32_t start = arg.start;

    populateSampleFeatures(sample_index);
}

void ofApp::populateSampleFeatures(uint32_t sample_index) {
    vector<Plotter>& feature_plots = plot_sample_features_[sample_index];
    for (Plotter& plot : feature_plots) { plot.reset(); }

    // 1. get samples
    MatrixDouble& sample = plot_samples_[sample_index].getData();

    // 2. get features by flowing samples through
    for (uint32_t i = 0; i < sample.getNumRows(); i++) {
        vector<double> data_point = sample.getRowVector(i);
        if (!pipeline_->preProcessData(data_point)) {
            ofLog(OF_LOG_ERROR) << "ERROR: Failed to compute features!";
        }

        // Last stage of feature extraction.
        uint32_t j = pipeline_->getNumFeatureExtractionModules();
        vector<double> feature = pipeline_->getFeatureExtractionData(j - 1);

        assert(feature.size() == feature_plots.size());
        for (uint32_t k = 0; k < feature_plots.size(); k++) {
            vector<double> feature_point = { feature[k] };
            feature_plots[k].push_back(feature_point);

            // sample_feature_ranges_[k].(first, second) tracks the min and max
            // for feature k so that the plots will be comparable.
            if (sample_feature_ranges_[k].first > feature[k]) {
                sample_feature_ranges_[k].first = feature[k];
            }
            if (sample_feature_ranges_[k].second < feature[k]) {
                sample_feature_ranges_[k].second = feature[k];
            }
        }
    }
    ofLog() << "Populating for index: " << sample_index
            << " with " << feature_plots[0].getData().getNumRows()
            << " data points.";
}

//void populateSampleFeatures(uint32_t sample_index, uint32_t start) {
//    assert(plot_sample_features_.size() == 1);
//
//    vector<Plotter> feature_plots = plot_sample_features_[sample_index];
//    MatrixDouble& sample = plot_samples_[sample_index].getData();
//
//    // need ways to get to know FFT feature information!
//    assert(false);
//}

void ofApp::savePipeline() {
    if (!pipeline_->save("pipeline.grt")) {
        ofLog(OF_LOG_ERROR) << "Failed to save the pipeline";
    }

    if (!pipeline_->getClassifier()->save("classifier.grt")) {
        ofLog(OF_LOG_ERROR) << "Failed to save the classifier";
    }
}

void ofApp::loadPipeline() {
    GRT::GestureRecognitionPipeline pipeline;
    if (!pipeline.load("pipeline.grt")) {
        ofLog(OF_LOG_ERROR) << "Failed to load the pipeline";
    }

    // TODO(benzh) Compare the two pipelines and warn the user if the
    // loaded one is different from his.
    (*pipeline_) = pipeline;
}

void ofApp::saveTrainingData() {
    if (!training_data_.save("training_data.grt")) {
        ofLog(OF_LOG_ERROR) << "Failed to save the training data";
    }
}

void ofApp::renameTrainingSample(int num) {
    int label = num + 1;

    rename_title_ = training_data_.getClassNameForCorrespondingClassLabel(label);
    if (rename_title_ == "CLASS_LABEL_NOT_FOUND") {
        // This means there is no data collected for this label.
        return;
    } else if (rename_title_ == "NOT_SET") {
        rename_title_ = "";
    }

    is_in_renaming_ = true;
    rename_target_ = label;
    display_title_ = rename_title_;
    plot_samples_[rename_target_ - 1].setTitle(display_title_);

    ofAddListener(ofEvents().update, this, &ofApp::updateEventReceived);
}

void ofApp::updateEventReceived(ofEventArgs& arg) {
    update_counter_++;

    // Assuming 60fps, to update the cursor every 0.1 seconds
    int period = 60 * 0.1;
    if (is_in_renaming_) {
        if (update_counter_ == period) {
            display_title_ = rename_title_ + "_";
        } else if (update_counter_ == period * 2) {
            display_title_ = rename_title_;
            update_counter_ = 0;
        }
        plot_samples_[rename_target_ - 1].setTitle(display_title_);
    }
}

void ofApp::deleteTrainingSample(int num) {
    int label = num + 1;

    // AFAICT, there's no way to delete an individual sample (except the
    // last one. Instead, remove all samples with the corresponding label
    // and add back all the ones except the one we want to delete.
    TimeSeriesClassificationData data = training_data_.getClassData(label);
    training_data_.eraseAllSamplesWithClassLabel(label);
    for (int i = 0; i < data.getNumSamples(); i++)
        if (i != plot_sample_indices_[num])
            training_data_.addSample(label, data[i].getData());

    if (data.getNumSamples() > 1) {
        // if we were showing the last sample, need to show previous one
        if (plot_sample_indices_[num] + 1 == data.getNumSamples()) {
            plot_sample_indices_[num]--;
            plot_samples_[num].setData(data[plot_sample_indices_[num]].getData());
        } else {
            plot_samples_[num].setData(data[plot_sample_indices_[num] + 1].getData());
        }
    } else {
        plot_samples_[num].reset();
        plot_sample_indices_[num] = -1;
    }

    populateSampleFeatures(num);
    should_save_training_data_ = true;
}

void ofApp::trimTrainingSample(int num) {
    int label = num + 1;
    TimeSeriesClassificationData data = training_data_.getClassData(label);
    training_data_.eraseAllSamplesWithClassLabel(label);

    pair<uint32_t, uint32_t> selection = plot_samples_[num].getSelection();
    for (int i = 0; i < data.getNumSamples(); i++) {
        if (i == plot_sample_indices_[num]) {
            GRT::MatrixDouble sample = data[i].getData();
            GRT::MatrixDouble new_sample;

            assert(selection.second - selection.first < sample.getNumRows());
            for (int row = selection.first; row < selection.second; row++) {
                new_sample.push_back(sample.getRowVector(row));
            }
            training_data_.addSample(label, new_sample);
            plot_samples_[num].setData(new_sample);
        } else {
            training_data_.addSample(label, data[i].getData());
        }
    }

    populateSampleFeatures(num);
    should_save_training_data_ = true;
}

void ofApp::relabelTrainingSample(int num) {
    // After this button is pressed, we enter relabel_mode
    is_in_relabeling_ = true;
    relabel_source_ = num + 1;
}

void ofApp::doRelabelTrainingSample(uint32_t source, uint32_t target) {
    if (source == target) {
        return;
    }

    // plot_samples_ (num) is 0-based, labels (source and target) are 1-based.
    uint32_t num = source - 1;
    MatrixDouble target_data;

    // Below is adapted from deleteTrainingSample. We delete sample first (need
    // to store it, that's why we can't just call deleteTrainingSample) and then
    // add it back with a different label.
    int label = num + 1;

    TimeSeriesClassificationData data = training_data_.getClassData(label);
    training_data_.eraseAllSamplesWithClassLabel(label);
    for (int i = 0; i < data.getNumSamples(); i++) {
        if (i != plot_sample_indices_[num]) {
            training_data_.addSample(label, data[i].getData());
        } else {
            target_data = data[i].getData();
            training_data_.addSample(target, target_data);
        }
    }

    if (data.getNumSamples() > 1) {
        // if we were showing the last sample, need to show previous one
        if (plot_sample_indices_[num] + 1 == data.getNumSamples()) {
            plot_sample_indices_[num]--;
            plot_samples_[num].setData(data[plot_sample_indices_[num]].getData());
        } else {
            plot_samples_[num].setData(data[plot_sample_indices_[num] + 1].getData());
        }
    } else {
        plot_samples_[num].reset();
        plot_sample_indices_[num] = -1;
    }

    populateSampleFeatures(num);
    should_save_training_data_ = true;

    // For the target label, update the plot.
    plot_sample_indices_[target - 1]++;
    plot_samples_[target - 1].setData(target_data);
    populateSampleFeatures(target - 1);
}

//--------------------------------------------------------------
void ofApp::update() {
    std::lock_guard<std::mutex> guard(input_data_mutex_);
    for (int i = 0; i < input_data_.getNumRows(); i++){
        vector<double> data_point = input_data_.getRowVector(i);

        if (pipeline_->getTrained()) {
            pipeline_->predict(data_point);
            predicted_label_ = pipeline_->getPredictedClassLabel();
            predicted_class_distances_ = pipeline_->getClassDistances();
            predicted_class_likelihoods_ = pipeline_->getClassLikelihoods();
            predicted_class_labels_ = pipeline_->getClassifier()->getClassLabels();

            if (ostream_ != NULL && predicted_label_ != 0) {
                ostream_->onReceive(predicted_label_);
            }
        }

        std::string title = training_data_.getClassNameForCorrespondingClassLabel(predicted_label_);
        if (title == "NOT_SET") title = std::string("Label") + std::to_string(predicted_label_);

        plot_inputs_.update(data_point, predicted_label_ != 0, title);

        if (istream_->hasStarted()) {
            if (!pipeline_->preProcessData(data_point)) {
                ofLog(OF_LOG_ERROR) << "ERROR: Failed to compute features!";
            }

            for (int j = 0; j < pipeline_->getNumPreProcessingModules(); j++) {
                vector<double> data = pipeline_->getPreProcessedData(j);
                plot_pre_processed_[j].update(data);
            }

            for (int j = 0; j < pipeline_->getNumFeatureExtractionModules(); j++) {
                // Working on j-th stage.
                vector<double> feature = pipeline_->getFeatureExtractionData(j);
                if (feature.size() < kTooManyFeaturesThreshold) {
                    for (int k = 0; k < feature.size(); k++) {
                        vector<double> v = { feature[k] };
                        plot_features_[j][k].update(v);
                    }
                } else {
                    assert(plot_features_[j].size() == 1);
                    plot_features_[j][0].setData(feature);
                }
            }
        }

        if (is_recording_) {
            if (label_ == 255) {
                plot_testdata_.update(data_point, predicted_label_ != 0, title);
            }
            sample_data_.push_back(data_point);
        }
    }
}

void ofDrawColoredBitmapString(ofColor color,
                               const string& text,
                               float x, float y) {
    ofPushStyle();
    ofSetColor(color);
    ofDrawBitmapString(text, x, y);
    ofPopStyle();
}

//--------------------------------------------------------------
void ofApp::draw() {
    // Hacky panel on the top.
    const uint32_t left_margin = 10;
    const uint32_t top_margin = 20;
    const uint32_t margin = 20;

    ofDrawBitmapString("[P]ipeline\t[T]raining\t[A]nalysis",
                       left_margin, top_margin);
    ofDrawLine(0, top_margin + 5, ofGetWidth(), top_margin + 5);
    ofColor red = ofColor(0xFF, 0, 0);
    switch (fragment_) {
        case PIPELINE:
            ofDrawColoredBitmapString(red, "[P]ipeline\t",
                                      left_margin, top_margin);
            drawLivePipeline();
            break;
        case TRAINING:
            ofDrawColoredBitmapString(red, "\t\t[T]raining",
                                      left_margin, top_margin);
            drawTrainingInfo();
            break;
        case ANALYSIS:
            ofDrawColoredBitmapString(red, "\t\t\t\t[A]nalysis",
                                      left_margin, top_margin);
            drawAnalysis();
            break;
        default:
            ofLog(OF_LOG_ERROR) << "Unknown tag!";
            break;
    }

    // Show instructions across all tabs.
    ofDrawBitmapString(kInstruction, left_margin, top_margin + margin);

    if (!gui_hide_) {
        gui_.draw();
    }
}

void ofApp::drawLivePipeline() {
    // A Pipeline was parsed in the ofApp::setup function and here we simple
    // draw the pipeline information.
    uint32_t margin = 30;
    uint32_t stage_left = 10;
    uint32_t stage_top = 70;
    uint32_t stage_height = // Hacky math for dimensions.
            (ofGetHeight() - margin) / (num_pipeline_stages_ + 1) - 2 * margin;
    uint32_t stage_width = ofGetWidth() - margin;

    // 1. Draw Input.
    ofPushStyle();
    plot_inputs_.draw(stage_left, stage_top, stage_width, stage_height);
    ofPopStyle();
    stage_top += stage_height + margin;

    // 2. Draw pre-processing: iterate all stages.
    for (int i = 0; i < pipeline_->getNumPreProcessingModules(); i++) {
        // working on pre-processing stage i.
        ofPushStyle();
        plot_pre_processed_[i].
                draw(stage_left, stage_top, stage_width, stage_height);
        ofPopStyle();
        stage_top += stage_height + margin;
    }

    // 3. Draw features.
    for (int i = 0; i < pipeline_->getNumFeatureExtractionModules(); i++) {
        // working on feature extraction stage i.
        ofPushStyle();
        uint32_t width = stage_width / plot_features_[i].size();
        for (int j = 0; j < plot_features_[i].size(); j++) {
            plot_features_[i][j].
                    draw(stage_left + j * width, stage_top,
                         width, stage_height);
        }
        ofPopStyle();
        stage_top += stage_height + margin;
    }
}

void ofApp::drawTrainingInfo() {
    uint32_t margin_left = 10;
    uint32_t margin_top = 70;
    uint32_t margin = 30;
    uint32_t stage_left = margin_left;
    uint32_t stage_top = margin_top;
    uint32_t stage_width = ofGetWidth() - margin;
    uint32_t stage_height = (ofGetHeight() - 200 - 4 * margin) / 2;

    // 1. Draw Input
    if (!is_in_feature_view_) {
        ofPushStyle();
        plot_inputs_.draw(stage_left, stage_top, stage_width, stage_height);
        ofPopStyle();
        stage_top += stage_height + margin;
    }

    // 2. Draw samples
    // Currently we support kNumMaxLabels_ labels
    uint32_t width = stage_width / kNumMaxLabels_;
    float minY = plot_inputs_.getRanges().first;
    float maxY = plot_inputs_.getRanges().second;
    auto class_tracker = training_data_.getClassTracker();

    for (int i = 0; i < kNumMaxLabels_; i++) {
        int label = i + 1;
        int x = stage_left + i * width;
        plot_samples_[i].setRanges(minY, maxY, true);
        plot_samples_[i].draw(x, stage_top, width, stage_height);

        for (int j = 0; j < class_tracker.size(); j++) {
            if (class_tracker[j].classLabel == label) {
                ofDrawBitmapString(
                    std::to_string(plot_sample_indices_[i] + 1) + " / " +
                    std::to_string(class_tracker[j].counter), x + width / 2 - 20,
                    stage_top + stage_height + 20);
                if (plot_sample_indices_[i] > 0)
                    ofDrawBitmapString("<-", x, stage_top + stage_height + 20);
                if (plot_sample_indices_[i] + 1 < class_tracker[j].counter)
                    ofDrawBitmapString("->", x + width - 20, stage_top + stage_height + 20);
                plot_sample_button_locations_[i].first.set(x, stage_top + stage_height, 20, 20);
                plot_sample_button_locations_[i].second.set(x + width - 20, stage_top + stage_height, 20, 20);
            }
        }

        // TODO(dmellis): only update these values when the screen size changes.
        training_sample_guis_[i]->setPosition(x + margin / 8, stage_top + stage_height + 30);
        training_sample_guis_[i]->setSize(width - margin / 4, training_sample_guis_[i]->getHeight());
        training_sample_guis_[i]->setWidthElements(width - margin / 4);
        training_sample_guis_[i]->draw();
    }

    stage_top += stage_height + 30 + training_sample_guis_[0]->getHeight();
    for (int i = 0; i < predicted_class_distances_.size() &&
                 i < predicted_class_likelihoods_.size(); i++) {
        ofColor backgroundColor, textColor;
        UINT label = predicted_class_labels_[i];
        if (predicted_label_ == label) {
            backgroundColor = ofColor(255);
            textColor = ofColor(0);
        } else {
            backgroundColor = ofGetBackgroundColor();
            textColor = ofColor(255);
        }
        ofDrawBitmapStringHighlight(
            std::to_string(predicted_class_distances_[i]).substr(0, 6),
            stage_left + (label - 1) * width,
            stage_top + margin,
            backgroundColor, textColor);
        ofDrawBitmapStringHighlight(
            std::to_string(predicted_class_likelihoods_[i]).substr(0, 6),
            stage_left + (label - 1) * width,
            stage_top + margin * 3 / 2,
            backgroundColor, textColor);
    }

    if (!is_in_feature_view_) { return; }
    // 3. Features
    stage_top += margin * 2;
    for (uint32_t i = 0; i < kNumMaxLabels_; i++) {
        uint32_t x = stage_left + i * width;
        uint32_t y = stage_top;
        vector<Plotter> feature_plots = plot_sample_features_[i];
        uint32_t margin = 5;
        uint32_t height = stage_height / feature_plots.size() - margin;

        for (uint32_t j = 0; j < feature_plots.size(); j++) {
            pair<uint32_t, uint32_t> range = sample_feature_ranges_[j];

            feature_plots[j].setRanges(range.first, range.second);
            feature_plots[j].draw(x, y, width, height);
            y += height + margin;
        }
    }
}

void ofApp::drawAnalysis() {
    uint32_t margin_left = 10;
    uint32_t margin_top = 70;
    uint32_t margin = 30;
    uint32_t stage_left = margin_left;
    uint32_t stage_top = margin_top;
    uint32_t stage_width = ofGetWidth() - margin;
    uint32_t stage_height = (ofGetHeight() - 200 - 4 * margin) / 2;

    // 1. Draw Input
    ofPushStyle();
    plot_inputs_.draw(stage_left, stage_top, stage_width, stage_height);
    ofPopStyle();
    stage_top += stage_height + margin;

    ofPushStyle();
    plot_testdata_.draw(stage_left, stage_top, stage_width, stage_height);
    ofPopStyle();
    stage_top += stage_height + margin;

}

void ofApp::exit() {
    if (training_thread_.joinable()) {
        training_thread_.join();
    }
    istream_->stop();

    // Save training data here!
    if (should_save_training_data_) {
        ofFileDialogResult result = ofSystemSaveDialog("TrainingData.grt",
                                                       "Save your training data?");
        if (result.bSuccess) {
            training_data_.save(result.getPath());
        }
    }

    // Clear all listeners.
    save_pipeline_button_.removeListener(this, &ofApp::savePipeline);
    load_pipeline_button_.removeListener(this, &ofApp::loadPipeline);
}

void ofApp::onDataIn(GRT::MatrixDouble input) {
    std::lock_guard<std::mutex> guard(input_data_mutex_);
    input_data_ = input;
}

//--------------------------------------------------------------
void ofApp::keyPressed(int key){
    if (is_in_renaming_) {
        // Add normal characters.
        if (key >= 32 && key <= 126) {
            // key code 32 is for space, we remap it to '_'.
            key = (key == 32) ? '_' : key;
            rename_title_ += key;
            return;
        }

        switch (key) {
          case OF_KEY_BACKSPACE:
            rename_title_ = rename_title_.substr(0, rename_title_.size() - 1);
            break;
          case OF_KEY_RETURN:
            training_data_.setClassNameForCorrespondingClassLabel(rename_title_,
                                                                  rename_target_);
            is_in_renaming_ = false;
            plot_samples_[rename_target_ - 1].setTitle(rename_title_);
            ofRemoveListener(ofEvents().update, this, &ofApp::updateEventReceived);
            return;
          default:
            break;
        }

        plot_samples_[rename_target_ - 1].setTitle(display_title_);
        return;
    }

    // If in relabeling, take action at key release stage.
    if (is_in_relabeling_) { return; }

    if (key >= '1' && key <= '9') {
        if (!is_recording_) {
            is_recording_ = true;
            label_ = key - '0';
            sample_data_.clear();
        }
    }

    switch (key) {
        case 'r':
            if (!is_recording_) {
                is_recording_ = true;
                label_ = 255;
                sample_data_.clear();
                test_data_.clear();
                plot_testdata_.reset();
            }
            break;
        case 'f': toggleFeatureView(); break;
        case 'h': gui_hide_ = !gui_hide_; break;
        case 'l': loadTrainingData(); break;
        case 'p': istream_->stop(); break;
        case 's': istream_->start(); break;
        case 't': trainModel(); break;

        // Tab related
        case 'P': fragment_ = PIPELINE; break;
        case 'T': fragment_ = TRAINING; break;
        case 'A': fragment_ = ANALYSIS; break;
    }
}

void ofApp::toggleFeatureView() {
    istream_->stop();
    is_in_feature_view_ = true;
    for (uint32_t i = 0; i < kNumMaxLabels_; i++) {
        populateSampleFeatures(i);
    }
}

void ofApp::trainModel() {
    // If prior training has not finished, we wait.
    if (training_thread_.joinable()) {
        training_thread_.join();
    }

    auto training_func = [this]() -> bool {
        ofLog() << "Training started";
        if (pipeline_->train(training_data_)) {
            ofLog() << "Training is successful";
            return true;
        } else {
            ofLog(OF_LOG_ERROR) << "Failed to train the model";
            return false;
        }
    };

    // TODO(benzh) Fix data race issue later.
    if (training_func()) {
        fragment_ = TRAINING;

        // Retest test data.
        plot_testdata_.reset();
        for (int i = 0; i < test_data_.getNumRows(); i++) {
            pipeline_->predict(test_data_.getRowVector(i));

            int predicted_label = pipeline_->getPredictedClassLabel();
            std::cout << predicted_label << " ";
            std::string title = training_data_.getClassNameForCorrespondingClassLabel(predicted_label);
            if (title == "NOT_SET") title = std::string("Label") + std::to_string(predicted_label);

            plot_testdata_.update(test_data_.getRowVector(i), predicted_label != 0, title);
        }

        pipeline_->reset();
    }
}

void ofApp::loadTrainingData() {
    GRT::TimeSeriesClassificationData training_data;
    ofFileDialogResult result = ofSystemLoadDialog("Load existing data", true);

    if (!result.bSuccess) return;

    if (!training_data.load(result.getPath()) ){
        ofLog(OF_LOG_ERROR) << "Failed to load the training data!"
                            << " path: " << result.getPath();
    }

    training_data_ = training_data;
    auto trackers = training_data_.getClassTracker();
    for (auto tracker : trackers) {
        plot_sample_indices_[tracker.classLabel - 1] = tracker.counter - 1;
    }

    for (int i = 0; i < training_data_.getNumSamples(); i++) {
        int label = training_data_[i].getClassLabel();
        plot_samples_[label - 1].setData(training_data_[i].getData());
        plot_samples_[label - 1].setTitle(
            training_data_.getClassNameForCorrespondingClassLabel(label));
    }

    // After we load the training data,
    should_save_training_data_ = false;

    trainModel();
}

//--------------------------------------------------------------
void ofApp::keyReleased(int key) {
    if (is_in_relabeling_ && key >= '1' && key <= '9') {
        doRelabelTrainingSample(relabel_source_, key - '0');
        is_in_relabeling_ = false;
        return;
    }

    is_recording_ = false;
    if (key >= '1' && key <= '9') {
        training_data_.addSample(label_, sample_data_);

        plot_samples_[label_ - 1].setData(sample_data_);
        plot_sample_indices_[label_ - 1] = training_data_.getClassTracker()[
                  training_data_.getClassLabelIndexValue(label_)].
                    counter - 1;

        should_save_training_data_ = true;
    }

    if (key == 'r') {
        test_data_ = sample_data_;
    }
}

//--------------------------------------------------------------
void ofApp::mouseMoved(int x, int y ) {

}

//--------------------------------------------------------------
void ofApp::mouseDragged(int x, int y, int button) {

}

//--------------------------------------------------------------
void ofApp::mousePressed(int x, int y, int button) {

}

//--------------------------------------------------------------
void ofApp::mouseReleased(int x, int y, int button) {
    for (int i = 0; i < kNumMaxLabels_; i++) {
        int label = i + 1;
        TimeSeriesClassificationData data = training_data_.getClassData(label);
        if (plot_sample_button_locations_[i].first.inside(x, y)) {
            if (plot_sample_indices_[i] > 0) {
                plot_sample_indices_[i]--;
                plot_samples_[i].setData(data[plot_sample_indices_[i]].getData());
                populateSampleFeatures(i);
            }
        }
        if (plot_sample_button_locations_[i].second.inside(x, y)) {
            if (plot_sample_indices_[i] + 1 < data.getNumSamples()) {
                plot_sample_indices_[i]++;
                plot_samples_[i].setData(data[plot_sample_indices_[i]].getData());
                populateSampleFeatures(i);
            }
        }
    }
}

//--------------------------------------------------------------
void ofApp::mouseEntered(int x, int y) {

}

//--------------------------------------------------------------
void ofApp::mouseExited(int x, int y) {

}

//--------------------------------------------------------------
void ofApp::windowResized(int w, int h) {
}

//--------------------------------------------------------------
void ofApp::gotMessage(ofMessage msg) {

}

//--------------------------------------------------------------
void ofApp::dragEvent(ofDragInfo dragInfo) {

}