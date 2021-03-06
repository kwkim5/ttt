#include <time.h>
#include <boost/filesystem.hpp>
#include <fstream>
#include <dlib/svm_threaded.h>
#include <dlib/svm.h>
#include <vector>

#include "mtcnn.h"
#include "kernels.h"

#include "gstCamera.h"
#include "glDisplay.h"
#include "loadImage.h"
#include "cudaRGB.h"
#include "cudaMappedMemory.h"

#include "face_embedder.h"
#include "face_classifier.h"
#include "alignment.h"
#include "videoSource.h"

#include "NetworkTCP.h"
#include "TcpSendRecvJpeg.h"

#define SPLIT_IMAGE_HANDLER

#ifdef SPLIT_IMAGE_HANDLER
#include "ImageHandler.h"
#endif // SPLIT_IMAGE_HANDLER

#define SPLIT_FACE_DECTOR
#ifdef SPLIT_FACE_DECTOR
#include "FaceDetector.h"
#endif // SPLIT_FACE_DECTOR

//#define EXCLUDE_RECOGNIZE_FACE true
#define MAX_THREAD_NUM  2

#ifndef SPLIT_IMAGE_HANDLER
// create high performace jetson camera - loads the image directly to gpu memory or shared memory
gstCamera* getCamera(){
    gstCamera* camera = gstCamera::Create(gstCamera::DefaultWidth, gstCamera::DefaultHeight, NULL);
	if( !camera ){
		printf("\nfailed to initialize camera device\n");
	}else{
        printf("\nsuccessfully initialized camera device\n");
        printf("    width:  %u\n", camera->GetWidth());
        printf("   height:  %u\n", camera->GetHeight());
            //start streaming
	    if( !camera->Open() ){
            printf("failed to open camera for streaming\n");
	    }else{
            printf("camera open for streaming\n");
        }
    }
    return camera;
}
#endif // SPLIT_IMAGE_HANDLER

// perform face recognition with Raspberry Pi camera
#ifdef MAX_THREAD_NUM
int camera_face_recognition(int id, int argc, char *argv[])
#else // MAX_THREAD_NUM
int camera_face_recognition(int argc, char *argv[])
#endif // MAX_THREAD_NUM
  {
   TTcpListenPort    *TcpListenPort;
   TTcpConnectedPort *TcpConnectedPort;
   struct sockaddr_in cli_addr;
   socklen_t          clilen;
   short              listen_port;
#ifndef SPLIT_IMAGE_HANDLER
   bool               usecamera=false;
#endif // SPLIT_IMAGE_HANDLER

    // -------------- Initialization -------------------
#ifndef SPLIT_FACE_DECTOR
    face_embedder embedder;                         // deserialize recognition network
    face_classifier classifier(&embedder);          // train OR deserialize classification SVM's
    if(classifier.need_restart() == 1) return 1;    // small workaround - if svms were trained theres some kind of memory problem when generate mtcnn
#endif // SPLIT_FACE_DECTOR

#ifndef SPLIT_IMAGE_HANDLER
    gstCamera* camera=NULL;
    videoSource* inputStream=NULL;
#endif // SPLIT_IMAGE_HANDLER
    bool user_quit = false;
    int imgWidth ;
    int imgHeight ;

   if (argc <2)
    {
       fprintf(stderr,"usage %s [port] [--input-codec=] [filename]\n", argv[0]);
       exit(0);
    }

    listen_port =atoi(argv[1]) + id;

#ifdef SPLIT_IMAGE_HANDLER
    imgWidth = ImageHandler::GetInstance()->GetImageWidth();
    imgHeight = ImageHandler::GetInstance()->GetImageHeight();
#else
    if (argc==2) usecamera=true;

    if (usecamera)
      {
        printf("video\n");

        camera = getCamera();                // create jetson camera - PiCamera. USB-Cam needs different operations in Loop!! not implemented!

        if( !camera )
         {
          printf("load camera failed\n");
          return -1;
         }

        imgWidth = camera->GetWidth();
        imgHeight = camera->GetHeight();
       }
    else
       {
        printf("video\n");
        inputStream = videoSource::Create(argc,argv,1);
        if( !inputStream )
         {
          printf("load video failed\n");
          return -1;
         }
        imgWidth = inputStream->GetWidth();
        imgHeight = inputStream->GetHeight();

       }
#endif // SPLIT_IMAGE_HANDLER

     mtcnn finder(imgHeight, imgWidth);              // build OR deserialize TensorRT detection network

    // malloc shared memory for images for access with cpu and gpu without copying data
    // cudaAllocMapped is used from jetson-inference
    uchar* rgb_gpu = NULL;
    uchar* rgb_cpu = NULL;
    cudaAllocMapped( (void**) &rgb_cpu, (void**) &rgb_gpu, imgWidth*imgHeight*3*sizeof(uchar) );
    uchar* cropped_buffer_gpu[2] = {NULL,NULL};
    uchar* cropped_buffer_cpu[2] = {NULL,NULL};
    cudaAllocMapped( (void**) &cropped_buffer_cpu[0], (void**) &cropped_buffer_gpu[0], 150*150*3*sizeof(uchar) );
    cudaAllocMapped( (void**) &cropped_buffer_cpu[1], (void**) &cropped_buffer_gpu[1], 150*150*3*sizeof(uchar) );

    // calculate fps
    double fps = 0.0;
    clock_t clk;
    clock_t point1;
    clock_t point2;
    clock_t point3;
    clock_t point4;
    clock_t now;

    // Detection vars
    int num_dets = 0;
    std::vector<std::string> label_encodings;       // vector for the real names of the classes/persons

#ifdef SPLIT_FACE_DECTOR
    FaceDetector::GetInstance()->GetLabelEncoding(&label_encodings);
#else
    // get the possible class names
    classifier.get_label_encoding(&label_encodings);
#endif // SPLIT_FACE_DECTOR

    printf("listen port:%d\n", listen_port);

   if  ((TcpListenPort=OpenTcpListenPort(listen_port))==NULL)  // Open TCP Network port
     {
       printf("OpenTcpListenPortFailed\n");
       return(-1);
     }


   clilen = sizeof(cli_addr);

   printf("Listening for connections\n");

   if  ((TcpConnectedPort=AcceptTcpConnection(TcpListenPort,&cli_addr,&clilen))==NULL)
     {
       printf("AcceptTcpConnection Failed\n");
       return(-1);
     }

   printf("Accepted connection Request\n");

// ------------------ "Detection" Loop -----------------------
    while(!user_quit){

        clk = clock();              // fps clock
	float* imgOrigin = NULL;    // camera image
        // the 2nd arg 1000 defines timeout, true is for the "zeroCopy" param what means the image will be stored to shared memory
#ifdef SPLIT_IMAGE_HANDLER
    imgOrigin = ImageHandler::GetInstance()->GetImageData();
        if (imgOrigin == NULL)
        {
           printf("origin capture image is null\n");
           continue;
        }
#else
#if 1
        if (usecamera)
         {
           if( !camera->CaptureRGBA(&imgOrigin, 1000, true))
           {
			printf("failed to capture RGBA image from camera\n");
			continue;
			}
         }
        else
         {

          if( !inputStream->Capture((float4**)&imgOrigin, 1000) ) continue;

         }
         if (imgOrigin == NULL)
         {
			printf("origin capture image is null\n");
            continue;
         }
#else
        float* imgCUDA=NULL;    //  image

        if( !loadImageRGBA("/home/lg/face_recognition_jetson_nano/faces/bbt/testdata/test/bbt_10.png", (float4**)&imgOrigin, (float4**)&imgCUDA, &imgWidth, &imgHeight))
        printf("failed to load image\n");

#endif
#endif // SPLIT_IMAGE_HANDLER

	point1 = clock();              // fps clock

        //since the captured image is located at shared memory, we also can access it from cpu
        // here I define a cv::Mat for it to draw onto the image from CPU without copying data -- TODO: draw from CUDA
        cudaRGBA32ToBGRA32(  (float4*)imgOrigin,  (float4*)imgOrigin, imgWidth, imgHeight); //ADDED DP
        cv::Mat origin_cpu(imgHeight, imgWidth, CV_32FC4, imgOrigin);

#ifndef EXCLUDE_RECOGNIZE_FACE
        // the mtcnn pipeline is based on GpuMat 8bit values 3 channels while the captured image is RGBA32
        // i use a kernel from jetson-inference to remove the A-channel and float to uint8
        cudaRGBA32ToRGB8( (float4*)imgOrigin, (uchar3*)rgb_gpu, imgWidth, imgHeight );

        // create GpuMat form the same image thanks to shared memory
        cv::cuda::GpuMat imgRGB_gpu(imgHeight, imgWidth, CV_8UC3, rgb_gpu);

        // pass the image to the MTCNN and get face detections
        std::vector<struct Bbox> detections;
        finder.findFace(imgRGB_gpu, &detections);

	point2 = clock();              // fps clock

        // check if faces were detected, get face locations, bounding boxes and keypoints
        std::vector<cv::Rect> rects;
        std::vector<float*> keypoints;
        num_dets = get_detections(origin_cpu, &detections, &rects, &keypoints);
        // if faces detected
        if(num_dets > 0){
            // crop and align the faces. Get faces to format for "dlib_face_recognition_model" to create embeddings
            std::vector<matrix<rgb_pixel>> faces;
            crop_and_align_faces(imgRGB_gpu, cropped_buffer_gpu, cropped_buffer_cpu, &rects, &faces, &keypoints);

            // generate face embeddings from the cropped faces and store them in a vector
            std::vector<matrix<float,0,1>> face_embeddings;
#ifdef SPLIT_FACE_DECTOR
            FaceDetector::GetInstance()->Embeddings(&faces, &face_embeddings);
#else
            embedder.embeddings(&faces, &face_embeddings);
#endif // SPLIT_FACE_DECTOR

	point3 = clock();              // fps clock

            // feed the embeddings to the pretrained SVM's. Store the predicted labels in a vector
            std::vector<double> face_labels;
#ifdef SPLIT_FACE_DECTOR
            FaceDetector::GetInstance()->Prediction(&face_embeddings, &face_labels);
#else
            classifier.prediction(&face_embeddings, &face_labels);
#endif // SPLIT_FACE_DECTOR

            // draw bounding boxes and labels to the original image
            draw_detections(origin_cpu, &rects, &face_labels, &label_encodings);
        }
        else
        {
            point3 = clock();              // fps clock
        }
#endif//  EXCLUDE_RECOGNIZE_FACE
            char str[256];
            sprintf(str, "TensorRT  %.1lf FPS", fps);               // print the FPS to the bar

            cv::putText(origin_cpu, str , cv::Point(0,20),
                    cv::FONT_HERSHEY_COMPLEX_SMALL, 1.0, cv::Scalar(0,0,0,255), 1 );
	point4 = clock();              // fps clock

        //Render captured image
        if (TcpSendImageAsJpeg(TcpConnectedPort,origin_cpu)<0)  break;

        // smooth FPS to make it readable
	now = clock();
        fps = (0.90 * fps) + (0.1 * (1 / ((double)(now-clk)/CLOCKS_PER_SEC)));
        //fps = (0.90 * fps) + (0.1 * (1 / ((double)(clock()-clk)/CLOCKS_PER_SEC)));
	printf("======== fps:%.1lf capture:%ldms findFace:%ldms detect2:%ldms prediction:%ldms send:%ldms =======\n",
			fps, (1000)*(point1-clk)/CLOCKS_PER_SEC, (1000)*(point2-point1)/CLOCKS_PER_SEC, (1000)*(point3-point2)/CLOCKS_PER_SEC, (1000)*(point4-point3)/CLOCKS_PER_SEC, (1000)*(now-point4)/CLOCKS_PER_SEC);
#ifdef SPLIT_IMAGE_HANDLER
    if (ImageHandler::GetInstance()->IsNotStreaming())
        break;
#else
        if ((inputStream) && (!inputStream->IsStreaming())) break;
#endif // SPLIT_IMAGE_HANDLER
    }

#ifndef SPLIT_IMAGE_HANDLER
    SAFE_DELETE(camera);
    SAFE_DELETE(inputStream);
#endif // SPLIT_IMAGE_HANDLER
    CHECK(cudaFreeHost(rgb_cpu));
    CHECK(cudaFreeHost(cropped_buffer_cpu[0]));
    CHECK(cudaFreeHost(cropped_buffer_cpu[1]));
    CloseTcpConnectedPort(&TcpConnectedPort); // Close network port;
    CloseTcpListenPort(&TcpListenPort);  // Close listen port
    return 0;
}


    // perform face recognition on test images
    int test_prediction_images(){

        // required format
        int wid = 1280;         // input size width
        int height = 720;       // input size height
        int channel = 3;        // all images rgb
        int face_size = 150;    // cropped faces are always square 150x150 px (required for dlib face embedding cnn)

        using namespace boost::filesystem;
        path p("faces/bbt/testdata/test/");     // directory with bbt-testdata

        // get recognition network and classifier
        face_embedder embedder;
        face_classifier classifier(&embedder);
        if(classifier.need_restart() == 1) return 1;

        // get detection network
        mtcnn finder(height, wid);
        int num_dets = 0;
        int num_images = 0;

        // get the possible class names
        std::vector<std::string> label_encodings;
        classifier.get_label_encoding(&label_encodings);

        // GPU memory for image in MTCNN format
        uchar* rgb_gpu = NULL;
        uchar* rgb_cpu = NULL;
        cudaAllocMapped( (void**) &rgb_cpu, (void**) &rgb_gpu, height*wid*channel*sizeof(uchar) );

        // GPU memory for cropped faces
        uchar* cropped_buffer_gpu[2] = {NULL,NULL};
        uchar* cropped_buffer_cpu[2] = {NULL,NULL};
        cudaAllocMapped( (void**) &cropped_buffer_cpu[0], (void**) &cropped_buffer_gpu[0], face_size*face_size*channel*sizeof(uchar) );
        cudaAllocMapped( (void**) &cropped_buffer_cpu[1], (void**) &cropped_buffer_gpu[1], face_size*face_size*channel*sizeof(uchar) );

        // gpu/cpu memory pointer to load image from disk
        float* imgCPU    = NULL;
        float* imgCUDA   = NULL;
        int    imgWidth  = 0;
        int    imgHeight = 0;

        // read and process images

        try
        {
            if (exists(p))
            {
                if (is_regular_file(p))
                    cout << p << "is a file" << endl;
                else if (is_directory(p))
                {
                    // Iteration over all images in the test directory
                    recursive_directory_iterator dir(p), end;
                    while(dir != end){

                        if(is_directory(dir->path())) {
                            cout << "enter: " << dir->path().filename().string() << endl;
                        }
                        // Handle the images
                        else {

                            // load image from disk to gpu/cpu-mem. (shared mem. for access without copying)
                            if( !loadImageRGBA(dir->path().string().c_str(), (float4**)&imgCPU, (float4**)&imgCUDA, &imgWidth, &imgHeight) )
                                printf("failed to load image '%s'\n", dir->path().filename().string().c_str());

                            // check if size fits
                            if((imgWidth != wid) || (imgHeight != height)){
                                cout << "image has wrong size!" << endl;
                            }else{
                                // create cv::Mat to draw detections from CPU (possible because of shared memory GPU/CPU)
                                cv::Mat origin_cpu(imgHeight, imgWidth, CV_32FC4, imgCPU);
                                // Convert image to format required by MTCNN
                                cudaRGBA32ToRGB8( (float4*)imgCUDA, (uchar3*)rgb_gpu, imgWidth, imgHeight );
                                // create GpuMat which is required by MTCNN pipeline
                                cv::cuda::GpuMat imgRGB_gpu(imgHeight, imgWidth, CV_8UC3, rgb_gpu);
                                std::vector<struct Bbox> detections;
                                // run MTCNN and get bounidng boxes of detected faces
                                finder.findFace(imgRGB_gpu, &detections);
                                std::vector<cv::Rect> rects;
                                std::vector<float*> keypoints;
                                num_dets = get_detections(origin_cpu, &detections, &rects, &keypoints);
                                if(num_dets > 0){
                                    // crop and align faces
                                    std::vector<matrix<rgb_pixel>> faces;
                                    crop_and_align_faces(imgRGB_gpu, cropped_buffer_gpu, cropped_buffer_cpu, &rects, &faces, &keypoints);
                                    // get face embeddings - feature extraction
                                    std::vector<matrix<float,0,1>> face_embeddings;
                                    embedder.embeddings(&faces, &face_embeddings);
                                    // do classification
                                    std::vector<double> face_labels;
                                    classifier.prediction(&face_embeddings, &face_labels);
                                    // draw detection bbox and classification to the image
                                    draw_detections(origin_cpu, &rects, &face_labels, &label_encodings);
                                }
                                CUDA(cudaDeviceSynchronize());

                                // save the predicted image
                                string outputFilename = "faces/bbt/testdata/result/" + to_string(num_images) + ".png";
                                if( !saveImageRGBA(outputFilename.c_str(), (float4*)imgCPU, imgWidth, imgHeight, 255) )
                                    printf("failed saving %ix%i image to '%s'\n", imgWidth, imgHeight, outputFilename.c_str());

                                num_images++;
                            }
                        }
                        CUDA(cudaDeviceSynchronize());
                        // free CUDA space to load next image
                        CUDA(cudaFreeHost(imgCPU));
                        ++dir;
                    }
                } else cout << p << " exists, but is neither a regular file nor a directory\n";
            } else cout << p << " does not exist\n";


        }
        catch (const filesystem_error& ex)
        {
            cout << ex.what() << '\n';
        }

        CHECK(cudaFreeHost(rgb_cpu));
        CHECK(cudaFreeHost(cropped_buffer_cpu[0]));
        CHECK(cudaFreeHost(cropped_buffer_cpu[1]));

        return 0;
    }



int main(int argc, char *argv[])
{

    int state = 0;

#ifdef SPLIT_IMAGE_HANDLER
    ImageHandler::GetInstance()->Initialize(argc, argv);
#endif // SPLIT_IMAGE_HANDLER

#ifdef SPLIT_FACE_DECTOR
    FaceDetector::GetInstance();
#endif // SPLIT_FACE_DECTOR

#ifdef MAX_THREAD_NUM
    std::thread    runner[MAX_THREAD_NUM];

    for (size_t i = 0; i < MAX_THREAD_NUM; ++i)
    {
        runner[i] = std::thread(camera_face_recognition, i, argc, argv);
        usleep(1000000);
    }

    for (size_t i = 0; i < MAX_THREAD_NUM; ++i)
    {
        runner[i].join();
    }
#else

    state = camera_face_recognition( argc, argv );
#endif // MAX_THREAD_NUM

    //else state = test_prediction_images(); //test prediction at a set of test images


    if(state == 1) cout << "Restart is required! Please type ./main again." << endl;

    return 0;
}
