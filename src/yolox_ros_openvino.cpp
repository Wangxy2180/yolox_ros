// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "yolox_ros/yolox_ros_openvino.h"

/**
 * @brief Define names based depends on Unicode path support
 */
#define tcout std::cout
#define file_name_t std::string
#define imread_t cv::imread
#define NMS_THRESH 0.45
#define BBOX_CONF_THRESH 0.3

static const int INPUT_W = 416;
static const int INPUT_H = 416;
static const int NUM_CLASSES = 80; // COCO has 80 classes. Modify this value on your own dataset.

YOLOXVINODetector::YOLOXVINODetector(/* args */) : nh_("~"),loop_cnt(0),fps_cnt(0)
{
    get_ros_param();
    // image_transport::ImageTransport it(nh_);
    // image_transport::Subscriber it_sub = it.subscribe("/imga",1,&YOLOXVINODetector::detect_callback,this);
    image_sub_ = nh_.subscribe("/img_source", 100, &YOLOXVINODetector::detect_callback, this);
    cout << model_path_ << endl;
    std::cout << "ctor" << std::endl;

    load_ie_model();
}

YOLOXVINODetector::~YOLOXVINODetector()
{
    cout << "dector" << endl;
}

void YOLOXVINODetector::get_ros_param()
{
    // nh_.param<std::string>("image_source", image_source_, "camera");
    // nh_.param<int>("cam_device", cam_device_, 0);
    // nh_.param<std::string>("video_path", video_path_, "video_path");
    nh_.param<std::string>("model_path", model_path_, "model_path");
    nh_.param<std::string>("infer_device", infer_device_, "infer_device");
}

void YOLOXVINODetector::load_ie_model()
{
    try
    {
        // --------------------------- Step 1. Initialize inference engine core
        // -------------------------------------
        // -----------------------------------------------------------------------------------------------------
        // Core ie;
        // Step 2. Read a model in OpenVINO Intermediate Representation (.xml and
        // .bin files) or ONNX (.onnx file) format
        CNNNetwork network = ie.ReadNetwork(model_path_);
        if (network.getOutputsInfo().size() != 1)
            throw std::logic_error("Sample supports topologies with 1 output only");
        if (network.getInputsInfo().size() != 1)
            throw std::logic_error("Sample supports topologies with 1 input only");
        // -----------------------------------------------------------------------------------------------------

        // --------------------------- Step 3. Configure input & output
        // ---------------------------------------------
        // --------------------------- Prepare input blobs
        // -----------------------------------------------------
        input_info = network.getInputsInfo().begin()->second;
        input_name = network.getInputsInfo().begin()->first;
        // name is "image"

        std::cout << "network name:" << network.getName() << std::endl;

        /* Mark input as resizable by setting of a resize algorithm.
         * In this case we will be able to set an input blob of any shape to an
         * infer request. Resize and layout conversions are executed automatically
         * during inference */
        //input_info->getPreProcess().setResizeAlgorithm(RESIZE_BILINEAR);
        //input_info->setLayout(Layout::NHWC);
        //input_info->setPrecision(Precision::FP32);

        // --------------------------- Prepare output blobs
        // ----------------------------------------------------
        if (network.getOutputsInfo().empty())
        {
            std::cerr << "Network outputs info is empty" << std::endl;
            return;
        }
        output_info = network.getOutputsInfo().begin()->second;
        output_name = network.getOutputsInfo().begin()->first;
        // name is "output"

        output_info->setPrecision(Precision::FP32);
        // -----------------------------------------------------------------------------------------------------

        // --------------------------- Step 4. Loading a model to the device
        // ------------------------------------------
        ExecutableNetwork executable_network = ie.LoadNetwork(network, infer_device_);
        // -----------------------------------------------------------------------------------------------------

        // --------------------------- Step 5. Create an infer request
        // -------------------------------------------------
        infer_request = executable_network.CreateInferRequest();
        // -----------------------------------------------------------------------------------------------------
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << '\n';
    }
}

void YOLOXVINODetector::detect_callback(const sensor_msgs::Image::ConstPtr &msg)
{
    Stopwatch timer;
    timer.tic();

    try
    {

        // --------------------------- Step 6. Prepare input
        // --------------------------------------------------------
        /* Read input image to a blob and set it to an infer request without resize
         * and layout conversions. */

        cv::Mat image;
        read_image(msg, image);
        cv::Mat pr_img = static_resize(image);
        Blob::Ptr imgBlob = infer_request.GetBlob(input_name); // just wrap Mat data by Blob::Ptr
        blobFromImage(pr_img, imgBlob);

        // infer_request.SetBlob(input_name, imgBlob);  // infer_request accepts input blob of any size
        // -----------------------------------------------------------------------------------------------------

        // --------------------------- Step 7. Do inference
        // --------------------------------------------------------
        /* Running the request synchronously */
        // timer.tic();

        infer_request.Infer();
        // -----------------------------------------------------------------------------------------------------

        // --------------------------- Step 8. Process output
        // ------------------------------------------------------
        const Blob::Ptr output_blob = infer_request.GetBlob(output_name);
        MemoryBlob::CPtr moutput = as<MemoryBlob>(output_blob);
        if (!moutput)
        {
            throw std::logic_error("We expect output to be inherited from MemoryBlob, "
                                   "but by fact we were not able to cast output to MemoryBlob");
        }
        // locked memory holder should be alive all time while access to its buffer
        // happens
        auto moutputHolder = moutput->rmap();
        const float *net_pred = moutputHolder.as<const PrecisionTrait<Precision::FP32>::value_type *>();

        int img_w = image.cols;
        int img_h = image.rows;
        float scale = std::min(INPUT_W / (image.cols * 1.0), INPUT_H / (image.rows * 1.0));
        std::vector<Object> objects;

        decode_outputs(net_pred, objects, scale, img_w, img_h);
        // draw_objects(image, objects);

        // -----------------------------------------------------------------------------------------------------
    }
    catch (const std::exception &ex)
    {
        std::cerr << ex.what() << std::endl;
        return;
    }
    double tmp_fps = 1.0/((timer.toc()) / 1000000000.0);
    loop_cnt+=1;
    fps_cnt+=tmp_fps;
    ROS_INFO("fps:%f", tmp_fps);
    ROS_INFO("avg fps:%f", fps_cnt/loop_cnt);
    return;
}

void YOLOXVINODetector::read_image(const sensor_msgs::Image::ConstPtr msgImage, cv::Mat &image)
{
    cv_bridge::CvImageConstPtr pCvImage;
    pCvImage = cv_bridge::toCvShare(msgImage, msgImage->encoding);
    pCvImage->image.copyTo(image);
}

cv::Mat YOLOXVINODetector::static_resize(cv::Mat &img)
{
    float r = std::min(INPUT_W / (img.cols * 1.0), INPUT_H / (img.rows * 1.0));
    // r = std::min(r, 1.0f);
    int unpad_w = r * img.cols;
    int unpad_h = r * img.rows;
    cv::Mat re(unpad_h, unpad_w, CV_8UC3);
    cv::resize(img, re, re.size());
    //cv::Mat out(INPUT_W, INPUT_H, CV_8UC3, cv::Scalar(114, 114, 114));
    cv::Mat out(INPUT_H, INPUT_W, CV_8UC3, cv::Scalar(114, 114, 114));
    re.copyTo(out(cv::Rect(0, 0, re.cols, re.rows)));
    return out;
}

void YOLOXVINODetector::blobFromImage(cv::Mat &img, Blob::Ptr &blob)
{
    int channels = 3;
    int img_h = img.rows;
    int img_w = img.cols;
    InferenceEngine::MemoryBlob::Ptr mblob = InferenceEngine::as<InferenceEngine::MemoryBlob>(blob);
    if (!mblob)
    {
        THROW_IE_EXCEPTION << "We expect blob to be inherited from MemoryBlob in matU8ToBlob, "
                           << "but by fact we were not able to cast inputBlob to MemoryBlob";
    }
    // locked memory holder should be alive all time while access to its buffer happens
    auto mblobHolder = mblob->wmap();

    float *blob_data = mblobHolder.as<float *>();

    for (size_t c = 0; c < channels; c++)
    {
        for (size_t h = 0; h < img_h; h++)
        {
            for (size_t w = 0; w < img_w; w++)
            {
                blob_data[c * img_w * img_h + h * img_w + w] =
                    (float)img.at<cv::Vec3b>(h, w)[c];
            }
        }
    }
}

void YOLOXVINODetector::generate_grids_and_stride(const int target_w, const int target_h, std::vector<int> &strides, std::vector<GridAndStride> &grid_strides)
{
    for (auto stride : strides)
    {
        int num_grid_w = target_w / stride;
        int num_grid_h = target_h / stride;
        for (int g1 = 0; g1 < num_grid_h; g1++)
        {
            for (int g0 = 0; g0 < num_grid_w; g0++)
            {
                grid_strides.push_back((GridAndStride){g0, g1, stride});
            }
        }
    }
}

void YOLOXVINODetector::generate_yolox_proposals(std::vector<GridAndStride> grid_strides, const float *feat_ptr, float prob_threshold, std::vector<Object> &objects)
{

    const int num_anchors = grid_strides.size();

    for (int anchor_idx = 0; anchor_idx < num_anchors; anchor_idx++)
    {
        const int grid0 = grid_strides[anchor_idx].grid0;
        const int grid1 = grid_strides[anchor_idx].grid1;
        const int stride = grid_strides[anchor_idx].stride;

        const int basic_pos = anchor_idx * (NUM_CLASSES + 5);

        // yolox/models/yolo_head.py decode logic
        //  outputs[..., :2] = (outputs[..., :2] + grids) * strides
        //  outputs[..., 2:4] = torch.exp(outputs[..., 2:4]) * strides
        float x_center = (feat_ptr[basic_pos + 0] + grid0) * stride;
        float y_center = (feat_ptr[basic_pos + 1] + grid1) * stride;
        float w = exp(feat_ptr[basic_pos + 2]) * stride;
        float h = exp(feat_ptr[basic_pos + 3]) * stride;
        float x0 = x_center - w * 0.5f;
        float y0 = y_center - h * 0.5f;

        float box_objectness = feat_ptr[basic_pos + 4];
        for (int class_idx = 0; class_idx < NUM_CLASSES; class_idx++)
        {
            float box_cls_score = feat_ptr[basic_pos + 5 + class_idx];
            float box_prob = box_objectness * box_cls_score;
            if (box_prob > prob_threshold)
            {
                Object obj;
                obj.rect.x = x0;
                obj.rect.y = y0;
                obj.rect.width = w;
                obj.rect.height = h;
                obj.label = class_idx;
                obj.prob = box_prob;

                objects.push_back(obj);
            }

        } // class loop

    } // point anchor loop
}

inline float YOLOXVINODetector::intersection_area(const Object &a, const Object &b)
{
    cv::Rect_<float> inter = a.rect & b.rect;
    return inter.area();
}

void YOLOXVINODetector::qsort_descent_inplace(std::vector<Object> &faceobjects, int left, int right)
{
    int i = left;
    int j = right;
    float p = faceobjects[(left + right) / 2].prob;

    while (i <= j)
    {
        while (faceobjects[i].prob > p)
            i++;

        while (faceobjects[j].prob < p)
            j--;

        if (i <= j)
        {
            // swap
            std::swap(faceobjects[i], faceobjects[j]);

            i++;
            j--;
        }
    }

#pragma omp parallel sections
    {
#pragma omp section
        {
            if (left < j)
                qsort_descent_inplace(faceobjects, left, j);
        }
#pragma omp section
        {
            if (i < right)
                qsort_descent_inplace(faceobjects, i, right);
        }
    }
}

void YOLOXVINODetector::qsort_descent_inplace(std::vector<Object> &objects)
{
    if (objects.empty())
        return;

    qsort_descent_inplace(objects, 0, objects.size() - 1);
}

void YOLOXVINODetector::nms_sorted_bboxes(const std::vector<Object> &faceobjects, std::vector<int> &picked, float nms_threshold)
{
    picked.clear();

    const int n = faceobjects.size();

    std::vector<float> areas(n);
    for (int i = 0; i < n; i++)
    {
        areas[i] = faceobjects[i].rect.area();
    }

    for (int i = 0; i < n; i++)
    {
        const Object &a = faceobjects[i];

        int keep = 1;
        for (int j = 0; j < (int)picked.size(); j++)
        {
            const Object &b = faceobjects[picked[j]];

            // intersection over union
            float inter_area = intersection_area(a, b);
            float union_area = areas[i] + areas[picked[j]] - inter_area;
            // float IoU = inter_area / union_area
            if (inter_area / union_area > nms_threshold)
                keep = 0;
        }

        if (keep)
            picked.push_back(i);
    }
}

void YOLOXVINODetector::decode_outputs(const float *prob, std::vector<Object> &objects, float scale, const int img_w, const int img_h)
{
    std::vector<Object> proposals;
    std::vector<int> strides = {8, 16, 32};
    std::vector<GridAndStride> grid_strides;

    generate_grids_and_stride(INPUT_W, INPUT_H, strides, grid_strides);
    generate_yolox_proposals(grid_strides, prob, BBOX_CONF_THRESH, proposals);
    qsort_descent_inplace(proposals);

    std::vector<int> picked;
    nms_sorted_bboxes(proposals, picked, NMS_THRESH);
    int count = picked.size();
    objects.resize(count);

    for (int i = 0; i < count; i++)
    {
        objects[i] = proposals[picked[i]];

        // adjust offset to original unpadded
        float x0 = (objects[i].rect.x) / scale;
        float y0 = (objects[i].rect.y) / scale;
        float x1 = (objects[i].rect.x + objects[i].rect.width) / scale;
        float y1 = (objects[i].rect.y + objects[i].rect.height) / scale;

        // clip
        x0 = std::max(std::min(x0, (float)(img_w - 1)), 0.f);
        y0 = std::max(std::min(y0, (float)(img_h - 1)), 0.f);
        x1 = std::max(std::min(x1, (float)(img_w - 1)), 0.f);
        y1 = std::max(std::min(y1, (float)(img_h - 1)), 0.f);

        objects[i].rect.x = x0;
        objects[i].rect.y = y0;
        objects[i].rect.width = x1 - x0;
        objects[i].rect.height = y1 - y0;
    }
}

void YOLOXVINODetector::draw_objects(const cv::Mat &bgr, const std::vector<Object> &objects)
{
    static const char *class_names[] = {
        "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck", "boat", "traffic light",
        "fire hydrant", "stop sign", "parking meter", "bench", "bird", "cat", "dog", "horse", "sheep", "cow",
        "elephant", "bear", "zebra", "giraffe", "backpack", "umbrella", "handbag", "tie", "suitcase", "frisbee",
        "skis", "snowboard", "sports ball", "kite", "baseball bat", "baseball glove", "skateboard", "surfboard",
        "tennis racket", "bottle", "wine glass", "cup", "fork", "knife", "spoon", "bowl", "banana", "apple",
        "sandwich", "orange", "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair", "couch",
        "potted plant", "bed", "dining table", "toilet", "tv", "laptop", "mouse", "remote", "keyboard", "cell phone",
        "microwave", "oven", "toaster", "sink", "refrigerator", "book", "clock", "vase", "scissors", "teddy bear",
        "hair drier", "toothbrush"};

    cv::Mat image = bgr.clone();

    for (size_t i = 0; i < objects.size(); i++)
    {
        const Object &obj = objects[i];

        fprintf(stderr, "%d = %.5f at %.2f %.2f %.2f x %.2f\n", obj.label, obj.prob,
                obj.rect.x, obj.rect.y, obj.rect.width, obj.rect.height);

        cv::Scalar color = cv::Scalar(color_list[obj.label][0], color_list[obj.label][1], color_list[obj.label][2]);
        float c_mean = cv::mean(color)[0];
        cv::Scalar txt_color;
        if (c_mean > 0.5)
        {
            txt_color = cv::Scalar(0, 0, 0);
        }
        else
        {
            txt_color = cv::Scalar(255, 255, 255);
        }

        cv::rectangle(image, obj.rect, color * 255, 2);

        char text[256];
        sprintf(text, "%s %.1f%%", class_names[obj.label], obj.prob * 100);

        int baseLine = 0;
        cv::Size label_size = cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, 0.4, 1, &baseLine);

        cv::Scalar txt_bk_color = color * 0.7 * 255;

        int x = obj.rect.x;
        int y = obj.rect.y + 1;
        //int y = obj.rect.y - label_size.height - baseLine;
        if (y > image.rows)
            y = image.rows;
        //if (x + label_size.width > image.cols)
        //x = image.cols - label_size.width;

        cv::rectangle(image, cv::Rect(cv::Point(x, y), cv::Size(label_size.width, label_size.height + baseLine)),
                      txt_bk_color, -1);

        cv::putText(image, text, cv::Point(x, y + label_size.height),
                    cv::FONT_HERSHEY_SIMPLEX, 0.4, txt_color, 1);
    }

    cv::imwrite("_demo.jpg", image);
    fprintf(stderr, "save vis file\n");
    // cv::imshow("image", image);
    // cv::waitKey(30);
}

int main(int argc, char *argv[])
{

    ros::init(argc, argv, "yolox_ros_openvino");

    YOLOXVINODetector *detector = new YOLOXVINODetector();

    std::cout << "end..." << std::endl;
    ros::spin();
    return 0;

    // try
    // {
    //     // ------------------------------ Parsing and validation of input arguments
    //     // ---------------------------------
    //     if (argc != 4)
    //     {
    //         tcout << "Usage : " << argv[0] << " <path_to_model> <path_to_image> <device_name>" << std::endl;
    //         return EXIT_FAILURE;
    //     }

    //     const file_name_t input_model{argv[1]};
    //     const file_name_t input_image_path{argv[2]};
    //     const std::string device_name{argv[3]};
    //     // -----------------------------------------------------------------------------------------------------

    //     // --------------------------- Step 1. Initialize inference engine core
    //     // -------------------------------------
    //     Core ie;
    //     // -----------------------------------------------------------------------------------------------------

    //     // Step 2. Read a model in OpenVINO Intermediate Representation (.xml and
    //     // .bin files) or ONNX (.onnx file) format
    //     CNNNetwork network = ie.ReadNetwork(input_model);
    //     if (network.getOutputsInfo().size() != 1)
    //         throw std::logic_error("Sample supports topologies with 1 output only");
    //     if (network.getInputsInfo().size() != 1)
    //         throw std::logic_error("Sample supports topologies with 1 input only");
    //     // -----------------------------------------------------------------------------------------------------

    //     // --------------------------- Step 3. Configure input & output
    //     // ---------------------------------------------
    //     // --------------------------- Prepare input blobs
    //     // -----------------------------------------------------
    //     InputInfo::Ptr input_info = network.getInputsInfo().begin()->second;
    //     std::string input_name = network.getInputsInfo().begin()->first;
    //     // name is "image"

    //     std::cout << "network name" << network.getName() << std::endl;

    //     /* Mark input as resizable by setting of a resize algorithm.
    //      * In this case we will be able to set an input blob of any shape to an
    //      * infer request. Resize and layout conversions are executed automatically
    //      * during inference */
    //     //input_info->getPreProcess().setResizeAlgorithm(RESIZE_BILINEAR);
    //     //input_info->setLayout(Layout::NHWC);
    //     //input_info->setPrecision(Precision::FP32);

    //     // --------------------------- Prepare output blobs
    //     // ----------------------------------------------------
    //     if (network.getOutputsInfo().empty())
    //     {
    //         std::cerr << "Network outputs info is empty" << std::endl;
    //         return EXIT_FAILURE;
    //     }
    //     DataPtr output_info = network.getOutputsInfo().begin()->second;
    //     std::string output_name = network.getOutputsInfo().begin()->first;
    //     // name is "output"

    //     output_info->setPrecision(Precision::FP32);
    //     // -----------------------------------------------------------------------------------------------------

    //     // --------------------------- Step 4. Loading a model to the device
    //     // ------------------------------------------
    //     ExecutableNetwork executable_network = ie.LoadNetwork(network, device_name);
    //     // -----------------------------------------------------------------------------------------------------

    //     // --------------------------- Step 5. Create an infer request
    //     // -------------------------------------------------
    //     InferRequest infer_request = executable_network.CreateInferRequest();
    //     // -----------------------------------------------------------------------------------------------------

    //     // --------------------------- Step 6. Prepare input
    //     // --------------------------------------------------------
    //     /* Read input image to a blob and set it to an infer request without resize
    //      * and layout conversions. */
    //     cv::Mat image = imread_t(input_image_path);
    //     cv::Mat pr_img = static_resize(image);
    //     Blob::Ptr imgBlob = infer_request.GetBlob(input_name); // just wrap Mat data by Blob::Ptr
    //     blobFromImage(pr_img, imgBlob);

    //     // infer_request.SetBlob(input_name, imgBlob);  // infer_request accepts input blob of any size
    //     // -----------------------------------------------------------------------------------------------------

    //     // --------------------------- Step 7. Do inference
    //     // --------------------------------------------------------
    //     /* Running the request synchronously */
    //     infer_request.Infer();
    //     // -----------------------------------------------------------------------------------------------------

    //     // --------------------------- Step 8. Process output
    //     // ------------------------------------------------------
    //     const Blob::Ptr output_blob = infer_request.GetBlob(output_name);
    //     MemoryBlob::CPtr moutput = as<MemoryBlob>(output_blob);
    //     if (!moutput)
    //     {
    //         throw std::logic_error("We expect output to be inherited from MemoryBlob, "
    //                                "but by fact we were not able to cast output to MemoryBlob");
    //     }
    //     // locked memory holder should be alive all time while access to its buffer
    //     // happens
    //     auto moutputHolder = moutput->rmap();
    //     const float *net_pred = moutputHolder.as<const PrecisionTrait<Precision::FP32>::value_type *>();

    //     int img_w = image.cols;
    //     int img_h = image.rows;
    //     float scale = std::min(INPUT_W / (image.cols * 1.0), INPUT_H / (image.rows * 1.0));
    //     std::vector<Object> objects;

    //     decode_outputs(net_pred, objects, scale, img_w, img_h);
    //     draw_objects(image, objects);

    //     // -----------------------------------------------------------------------------------------------------
    // }
    // catch (const std::exception &ex)
    // {
    //     std::cerr << ex.what() << std::endl;
    //     return EXIT_FAILURE;
    // }
    // return EXIT_SUCCESS;
}
