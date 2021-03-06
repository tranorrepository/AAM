#include "aammodel.h"
#include "utils.h"
#include "ioutils.h"
#include "robust_pca/robust_pca.h"

namespace aam {
  using namespace std;

  using Eigen::VectorXd;
  using Eigen::MatrixXd;

  using cv::Mat;
  namespace fs = boost::filesystem;

  AAMModel::AAMModel() {
    Init();
  }

  AAMModel::AAMModel(const std::vector<QImage>& images, const std::vector<Mat>& points)
    : input_images(images), input_points(points) {

    Init();
    Preprocess();
  }

  namespace {
    inline void safe_create(const fs::path& p, bool force_remove=true) {
      if (fs::exists(p)) {
        if (force_remove) {
          fs::remove_all(p);
        }
      }

      fs::create_directory(p);
    }
  }

  void AAMModel::Init() {
    metric = TextureError;

    triangles = LoadTriangulation("/home/phg/Data/Multilinear/landmarks_triangulation.dat");
    // Convert to 0-based indexing
    std::for_each(triangles.begin(), triangles.end(), [](cv::Vec3i& v) { v -= cv::Vec3i(1, 1, 1); });

    // Use current directory as default path
    output_path = "./";

    // Create output directories if not exists
    safe_create(fs::path(output_path) / fs::path("outliers"));
    safe_create(fs::path(output_path) / fs::path("inliers"));
  }

  void AAMModel::SetImages(const std::vector<QImage> &images) {
    input_images = images;
  }

  void AAMModel::SetPoints(const std::vector<cv::Mat> &points) {
    input_points = points;
  }

  void AAMModel::SetOutputPath(const std::string &path) {
    output_path = path;

    cout << "Output path is " << output_path << endl;

    // Create output directories if not exists
    safe_create(fs::path(output_path), false);
    safe_create(fs::path(output_path) / fs::path("outliers"));
    safe_create(fs::path(output_path) / fs::path("inliers"));
  }

  void AAMModel::ProcessImages() {
    const int nimages = input_images.size();

    // Convert input images to opencv Mat
    images.resize(nimages);
    for(int i=0;i<nimages;++i) {
      images[i] = QImage2CVMat(input_images[i]);

#if 0
      // For debugging
    cv::imshow("image", images[i]);
    cv::waitKey();
#endif
    }
  }

  void AAMModel::ProcessShapes() {
    const int nimages = input_images.size();
    const int npoints = input_points.front().rows;

    // Collect all input shapes
    shapes = Mat(nimages, npoints*2, CV_64FC1);
    for(int i=0;i<nimages;++i) {
      Mat ptsT = input_points[i];
      Mat row_i = ptsT.reshape(1, 1).clone();
      row_i.copyTo(shapes.row(i));

#if 0
      // For debugging
    cout << shapes.row(i) << endl;
    Mat img_i = images[i].clone();
    DrawShape(img_i, shapes.row(i));
    cv::imshow("image", img_i);
    cv::waitKey();
#endif
    }
  }

  void AAMModel::InitializeMeanShapeAndTexture() {
    // Compute mean shape
    meanshape = ComputeMeanShape(shapes);

#if 0
    // For debugging
  cout << "Drawing mesh ..." << endl;
  Mat img(250, 250, CV_8UC3, cv::Scalar(0, 0, 0));
  DrawMesh(img, triangles, meanshape);
  DrawShape(img, meanshape);
  cv::imshow("meanshape", img);
  cv::waitKey();
#endif

    // Compute mean texture
    meantexture = ComputeMeanTexture(images, shapes, meanshape);

#if 0
    cv::namedWindow("mean texture", cv::WINDOW_NORMAL);
  Mat img(images.front().rows, images.front().cols, images.front().type(), cv::Scalar(0, 0, 0));
  FillImage(meantexture + 0.5, pixel_coords, img);
  DrawMesh(img, triangles, meanshape);
  DrawShape(img, meanshape);
  cv::imshow("mean texture", img);
  cv::waitKey();
#endif
  }

  void AAMModel::Preprocess() {
    boost::timer::auto_cpu_timer t("Preprocessing finished in %w seconds.\n");

    ProcessImages();

    ProcessShapes();

    InitializeMeanShapeAndTexture();
  }

  Mat AAMModel::AlignShape(const Mat& from_shape, const Mat& to_shape) {
    Mat aligned_shape;

#if 0
    cout << "from: " << from_shape << endl;
  cout << "to: " << to_shape << endl;
  PAUSE();
#endif

    auto from_points = CVMat2Points(from_shape);
    auto to_points = CVMat2Points(to_shape);

#if 0
    Mat tform = cv::estimateRigidTransform(from_points, to_points, false);
#else
    Mat tform = EstimateRigidTransform(from_shape, to_shape);
#endif

#if 0
    cout << tform << endl;
  PAUSE();
#endif

    cv::transform(from_shape.reshape(2), aligned_shape, tform);
    aligned_shape = aligned_shape.reshape(1);

#if 0
    cout << aligned_shape << endl;
  PAUSE();
#endif


    return aligned_shape;
  }

  Mat AAMModel::ScaleShape(const Mat& shape, double size) {
    const int npoints = shape.cols / 2;

    Mat s = shape.clone();

    double max_x = -1e6;
    double min_x = 1e6;
    double max_y = -1e6;
    double min_y = 1e6;

    for(int j=0;j<npoints;++j) {
      double x = shape.at<double>(0, j*2);
      double y = shape.at<double>(0, j*2+1);

      max_x = max(max_x, x); min_x = min(min_x, x);
      max_y = max(max_y, y); min_y = min(min_y, y);
    }

    double center_x = 0.5 * (max_x + min_x);
    double center_y = 0.5 * (max_y + min_y);

    double xrange = max_x - min_x;
    double yrange = max_y - min_y;

    double factor = 0.95 * size / max(xrange, yrange);

    for(int j=0;j<npoints;++j) {
      double x = s.at<double>(0, j*2);
      double y = s.at<double>(0, j*2+1);

      s.at<double>(0, j*2) = (x - center_x) * factor + size * 0.5;
      s.at<double>(0, j*2+1) = (y - center_y) * factor + size * 0.5;
    }
    return s;
  }

  Mat AAMModel::ComputeMeanShape(const Mat& shapes) {
    const int npoints = shapes.cols / 2;
    const int nimages = images.size();

    const int target_shape_size = 250;

    Mat meanshape = Mat::zeros(1, npoints*2, CV_64FC1);
    for(int j=0;j<nimages;++j) meanshape += shapes.row(j);
    meanshape /= nimages;
    meanshape = ScaleShape(meanshape, target_shape_size);

    const int max_iters = 100;

    for(int iter=0;iter<max_iters;++iter) {
      Mat new_meanshape = Mat::zeros(1, npoints*2, CV_64FC1);
      for(int j=0;j<nimages;++j) {
        new_meanshape = new_meanshape + AlignShape(shapes.row(j), meanshape);
      }
      new_meanshape /= nimages;
      new_meanshape = ScaleShape(new_meanshape, target_shape_size);
      double norm = cv::norm(new_meanshape - meanshape);
      cout << "iter " << iter << ": Diff = " << norm << endl;
      meanshape = new_meanshape;

#if 0
      Mat img(250, 250, CV_8UC3, cv::Scalar(0, 0, 0));
    DrawShapeWithIndex(img, meanshape);
    cv::imshow("meanshape", img);
    cv::waitKey();
#endif

      if(norm < 1e-3) break;
    }

    return meanshape;
  }

  Mat AAMModel::ComputeMeanTexture(const vector<Mat>& images,
                                   const Mat& shapes,
                                   const Mat& meanshape) {

    const int nimages = images.size();
    const int npoints = meanshape.cols / 2;
    const int ntriangles = triangles.size();
    const int w = images.front().cols;
    const int h = images.front().rows;

    auto get_point = [&](const Mat& shape, int idx) {
      return cv::Point2f(shape.at<double>(0, idx*2),
                         shape.at<double>(0, idx*2+1));
    };

    auto shape_to_verts = [&get_point](const Mat& shape) {
      const int npoints = shape.cols / 2;
      vector<cv::Point2f> verts(npoints);
      for(int i=0;i<npoints;++i) {
        verts[i] = get_point(shape, i);
      }
      return verts;
    };

    vector<cv::Point2f> meanshape_verts = shape_to_verts(meanshape);
    vector<vector<cv::Point2f>> shape_verts(nimages);
    for(int i=0;i<nimages;++i) shape_verts[i] = shape_to_verts(shapes.row(i));

    tforms.resize(nimages, vector<Mat>(ntriangles));
    tforms_inv.resize(nimages, vector<Mat>(ntriangles));

    for(int j=0;j<ntriangles;++j) {
      const int vj0 = triangles[j][0];
      const int vj1 = triangles[j][1];
      const int vj2 = triangles[j][2];

      cv::Point2f mv0 = meanshape_verts[vj0];
      cv::Point2f mv1 = meanshape_verts[vj1];
      cv::Point2f mv2 = meanshape_verts[vj2];

      for(int i=0;i<nimages;++i) {
        cv::Point2f v0 = get_point(shapes.row(i), vj0);
        cv::Point2f v1 = get_point(shapes.row(i), vj1);
        cv::Point2f v2 = get_point(shapes.row(i), vj2);

        tforms[i][j] = cv::getAffineTransform(vector<cv::Point2f>{v0, v1, v2},
                                              vector<cv::Point2f>{mv0, mv1, mv2});
        cv::invertAffineTransform(tforms[i][j], tforms_inv[i][j]);
      }
    }

    // Create pixel map in the texture space
    auto GeneratePixelMap = [=](const std::vector<cv::Point2f>& verts, const std::vector<cv::Vec3i>& triangles, cv::Mat& pixel_map) {
      const int ntriangles = triangles.size();
      pixel_map = Mat(h, w, CV_8UC1, cv::Scalar(0));
      for(int j=0;j<ntriangles;++j) {
        const int vj0 = triangles[j][0];
        const int vj1 = triangles[j][1];
        const int vj2 = triangles[j][2];

        FillTriangle(pixel_map, verts[vj0], verts[vj1], verts[vj2], cv::Scalar(j+tri_id_offset));
      }
    };

    GeneratePixelMap(meanshape_verts, triangles, pixel_map);
#if 0
    cv::imshow("mean pixel map", pixel_map);
    cv::waitKey();
#endif

    inv_pixel_maps.resize(nimages);
    for(int i=0;i<nimages;++i) {
      GeneratePixelMap(shape_verts[i], triangles, inv_pixel_maps[i]);
#if 0
      cv::imshow("pixel map", inv_pixel_maps[i]);
      cv::waitKey();
#endif
    }

    // Count the number of pixels we need to process
    auto collect_pixel_info = [=](const Mat& pix_map,
                                  vector<int>& pix_counts,
                                  vector<vector<cv::Vec2i>>& pix_coords,
                                  vector<Mat>& pix_mats){
      pix_counts.resize(ntriangles, 0);
      pix_coords.resize(ntriangles);
      for (int i = 0; i < h; ++i) {
        for (int j = 0; j < w; ++j) {
          int tri_id = static_cast<int>(pix_map.at<unsigned char>(i, j)) - tri_id_offset;
          if (tri_id >= 0) {
            ++pix_counts[tri_id];
            pix_coords[tri_id].push_back(cv::Vec2i(i, j));
          }
        }
      }

      // Create the list of points we need to project back/forward
      pix_mats.resize(ntriangles);
      for(int j=0;j<ntriangles;++j) {
        pix_mats[j] = cv::Mat(pix_counts[j], 2, CV_32FC1);
        for(int k=0;k<pix_counts[j];++k) {
          auto pix_coord = pix_coords[j][k];
          pix_mats[j].at<float>(k, 0) = pix_coord[1];
          pix_mats[j].at<float>(k, 1) = pix_coord[0];
        }
      }
    };

    collect_pixel_info(pixel_map, pixel_counts, pixel_coords, pixel_mats);

    inv_pixel_mats.resize(nimages);
    inv_pixel_counts.resize(nimages);
    inv_pixel_coords.resize(nimages);
    for(int i=0;i<nimages;++i) collect_pixel_info(inv_pixel_maps[i], inv_pixel_counts[i], inv_pixel_coords[i], inv_pixel_mats[i]);

    // Create image space points to texture space points mapping
    inv_pixel_pts.resize(nimages, vector<Mat>(ntriangles));
    for(int i=0;i<nimages;++i) {
#if 0
      Mat img(h, w, CV_64FC3, cv::Scalar(0, 0, 0));
#endif
      for(int j=0;j<ntriangles;++j) {
        if(inv_pixel_mats[i][j].rows == 0) {
          continue;
        }

        // project the points from input image to texture space
        cv::Mat pts;
        cv::transform(inv_pixel_mats[i][j].reshape(2), pts, tforms[i][j]);
        pts = pts.reshape(1, 1);

        inv_pixel_pts[i][j] = pts;

#if 0
        for(int k=0;k<inv_pixel_mats[i][j].rows;++k) {
          cv::circle(img, cv::Point(inv_pixel_mats[i][j].at<float>(k, 0),
                                    inv_pixel_mats[i][j].at<float>(k, 1)),
          1, cv::Scalar(0, 255, 0));
        }
#endif
      }
#if 0
      cv::imshow("pixel map", inv_pixel_maps[i]);
      cv::imshow("source points", img);
      cv::waitKey();
#endif
    }

    // Warp the input images to the meanshape space
    warped_images.resize(nimages);

    for(int i=0;i<nimages;++i) {
#if 0
      warped_images[i] = Mat(h, w, CV_64FC3, cv::Scalar(0, 0, 0));
      for(int j=0;j<ntriangles;++j) {
        // project back the points to input image space
        cv::Mat pts;
        cv::transform(pixel_mats[j].reshape(2), pts, tforms_inv[i][j]);
        pts = pts.reshape(1, 1);

        for(int k=0;k<pixel_counts[j];++k) {
          auto pix_coord = pixel_coords[j][k];

          cv::Vec3d sample = SampleImage(images[i], cv::Point2f(pts.at<float>(0,k*2), pts.at<float>(0,k*2+1)));

          warped_images[i].at<cv::Vec3d>(pix_coord[0], pix_coord[1]) = sample;
        }
      }
#else
      warped_images[i] = WarpImage(images[i], tforms_inv[i], pixel_mats, pixel_coords);
#endif

#if 0
      cout << i << endl;
      cout << tforms.size() << ", " << inv_pixel_mats.size() << ", " << inv_pixel_coords.size() << endl;
      cv::imshow("warped", warped_images[i]);
      Mat warp_back = WarpImage(warped_images[i], tforms[i], inv_pixel_mats[i], inv_pixel_coords[i]);
      cv::imshow("warped back", warp_back);
      cv::waitKey();
#endif
    }

    // Put all texels into a Mat
    int ntexels = accumulate(pixel_counts.begin(), pixel_counts.end(), 0);
    textures = Mat(nimages, ntexels, CV_64FC3);
    for(int i=0;i<nimages;++i) {
      int offset = 0;
      for(int j=0;j<pixel_counts.size();++j) {
        for(int k=0;k<pixel_coords[j].size();++k) {
          // collect the texels
          auto pix_coord = pixel_coords[j][k];
          textures.at<cv::Vec3d>(i, offset+k) = warped_images[i].at<cv::Vec3d>(pix_coord[0], pix_coord[1]);
        }
        offset += pixel_counts[j];
      }
    }

#if 0
    Mat mean_warped_image(h, w, CV_64FC3, cv::Scalar(0, 0, 0));
    for(int i=0;i<nimages;++i) {
      mean_warped_image += warped_images[i];
      cout << i << endl;
      cv::imshow("warped", warped_images[i]);
      cv::imshow("meantexture", mean_warped_image / (i+1));
      cv::waitKey();
    }
    mean_warped_image /= nimages;
#endif

    Mat meantexture;
    cv::reduce(textures, meantexture, 0, CV_REDUCE_AVG);

    // Iteratively compute the mean texture
    const int max_iters = 100;
    normalized_textures = Mat(nimages, ntexels, CV_64FC3);

    for(int iter=0;iter<max_iters;++iter) {
      Mat newmeantexture(1, ntexels, CV_64FC3, cv::Scalar(0, 0, 0));
      for(int i=0;i<nimages;++i) {
        auto normalization_res = NormalizeTextureVec(textures.row(i), meantexture);
        normalized_textures.row(i) = std::get<0>(normalization_res)*1;
        newmeantexture += normalized_textures.row(i);
      }
      newmeantexture /= nimages;

      double diff_iter = cv::norm(newmeantexture - meantexture);
      printf("iter %d: %.6f, %.6f\n", iter, diff_iter, cv::norm(newmeantexture));
      if(diff_iter < 1e-6) break;
      const double lambda = 0.75;
      meantexture = lambda * newmeantexture + (1.0 - lambda) * meantexture;
    }

    // subtract mean texture from the normalized textures
    for(int i=0;i<nimages;++i) normalized_textures.row(i) -= meantexture;

    return meantexture;
  }

  void AAMModel::BuildModel(vector<int> indices) {
    if(indices.empty()) {
      indices.resize(input_images.size());
      std::iota(indices.begin(), indices.end(), 0);
    }

    int nimages = indices.size();

    // Construct shape and texture model with the provided indices
    cv::PCA shape_model, texture_model;
    {
      boost::timer::auto_cpu_timer t("Shape model constructed in %w seconds.\n");
      shape_model = shape_model(shapes, Mat(), CV_PCA_DATA_AS_ROW, 0.98);
    }
    {
      boost::timer::auto_cpu_timer t("Texture model constructed in %w seconds.\n");
      texture_model = texture_model(normalized_textures.reshape(1), Mat(), CV_PCA_DATA_AS_ROW, 0.98);
    }

    Mat diffs(1, nimages, CV_64FC1);
    vector<Mat> reconstructions(nimages);
    for(int i=0;i<nimages;++i) {
      Mat coeffs(1, texture_model.mean.cols, texture_model.mean.type()), reconstructed;
      Mat vec = textures.row(i);

      // normalize it
      Mat normalized_vec, beta_i;
      double alpha_i;

      tie(normalized_vec, alpha_i, beta_i) = NormalizeTextureVec(vec, meantexture);
      normalized_vec -= meantexture;

      texture_model.project(normalized_vec.reshape(1), coeffs);
      texture_model.backProject(coeffs, reconstructed);
      reconstructed = reconstructed.reshape(3);

      diffs.at<double>(0, i) = cv::norm(normalized_vec, reconstructed, cv::NORM_L2);

      // unnormalize it
      reconstructed = (reconstructed + meantexture) * alpha_i + beta_i.reshape(3, 1);

      reconstructions[i] = reconstructed;
      printf("%d. diff = %g\n", i, diffs.at<double>(0, i));

#if 1
      cv::Mat img(images.front().rows, images.front().cols, images.front().type(), cv::Scalar(0, 0, 0));
      FillImage(reconstructions[i], pixel_coords, img);
      cv::imshow("outlier", img);

      cv::Mat img_ref(images.front().rows, images.front().cols, images.front().type(), cv::Scalar(0, 0, 0));
      FillImage(textures.row(i), pixel_coords, img_ref);
      cv::imshow("ref", img_ref);
      cv::waitKey();
#endif
    }

    cv::Scalar mean_diff, stddev_diff;
    cv::meanStdDev(diffs, mean_diff, stddev_diff);

    cout << mean_diff << ", " << stddev_diff << endl;

    for(int i=0;i<nimages;++i) {
      if(diffs.at<double>(0, i) >= mean_diff[0] + 3 * stddev_diff[0]) {
        int max_idx = i;
        // Fill the image
        cv::Mat img(images.front().rows, images.front().cols, images.front().type(), cv::Scalar(0, 0, 0));
        FillImage(reconstructions[max_idx], pixel_coords, img);
        cv::imshow("outlier", img);

        cv::Mat img_ref(images.front().rows, images.front().cols, images.front().type(), cv::Scalar(0, 0, 0));
        FillImage(textures.row(max_idx) + meantexture, pixel_coords, img_ref);
        cv::imshow("ref", img_ref);
        cv::waitKey();
      }
    }
  }

  vector<int> AAMModel::FindInliers(vector<int> indices) {
    if(indices.empty()) {
      indices.resize(input_images.size());
      std::iota(indices.begin(), indices.end(), 0);
    }

    int nimages = indices.size();

    set<int> current_set(indices.begin(), indices.end());

    vector<Mat> reconstructions(nimages), fitted_images(nimages);
    Mat diffs(1, nimages, CV_64FC1);

    #pragma omp parallel for
    for(int i=0;i<indices.size();++i) {
      cout << i << endl;
      set<int> set_i = current_set;
      set_i.erase(indices[i]);

      Mat shapes_i(set_i.size(), shapes.cols, shapes.type()),
        normalized_textures_i(set_i.size(), normalized_textures.cols, normalized_textures.type());

      int ridx = 0;
      for(auto j : set_i) {
        shapes_i.row(ridx) = shapes.row(j) * 1;
        normalized_textures_i.row(ridx) = normalized_textures.row(j) * 1;
        ++ridx;
      }
      Mat normalized_textures_i_reshaped = normalized_textures_i.reshape(1);

      // Construct shape and texture model with the provided indices
      cv::PCA shape_model, texture_model;
      {
        boost::timer::auto_cpu_timer t("Shape model constructed in %w seconds.\n");
        shape_model = shape_model(shapes_i, Mat(), CV_PCA_DATA_AS_ROW, 0.98);
      }
      {
        boost::timer::auto_cpu_timer t("Texture model constructed in %w seconds.\n");
        texture_model = texture_model(normalized_textures_i_reshaped,
                                      Mat(),
                                      CV_PCA_DATA_AS_ROW,
                                      0.98);
      }

      PrintShape(texture_model.eigenvectors);

      Mat coeffs(1, texture_model.mean.cols, texture_model.mean.type()), reconstructed;
      Mat vec = textures.row(indices[i]);

      // normalize it
      Mat normalized_vec, beta_i;
      double alpha_i;
      tie(normalized_vec, alpha_i, beta_i) = NormalizeTextureVec(vec, meantexture);
      // subtract meantexture since the PCA model is built on difference
      normalized_vec -= meantexture;

      texture_model.project(normalized_vec.reshape(1), coeffs);
      texture_model.backProject(coeffs, reconstructed);
      reconstructed = reconstructed.reshape(3);

      // unnormalize it
      reconstructions[i] = (reconstructed + meantexture) * alpha_i + beta_i.reshape(3, 1);

      Mat fitted, warp_back;
      switch(metric) {
        case TextureError: {
          diffs.at<double>(0, i) = cv::norm(normalized_vec, reconstructed, cv::NORM_L2);
          break;
        }
        case FittingError: {
          // Warp reconstructed back to image space and compute fitting error using the pixel mask
          double diff_i = 0;
          cout << "warping image ..." << endl;
          fitted = Mat(images.front().rows, images.front().cols, images.front().type(), cv::Scalar(0, 0, 0));
          FillImage(reconstructions[i], pixel_coords, fitted);
          warp_back = WarpImage(fitted, tforms[indices[i]], inv_pixel_mats[indices[i]], inv_pixel_coords[indices[i]]);
          fitted_images[i] = warp_back;
          diffs.at<double>(0, i) = ComputeRMSE(warp_back, images[indices[i]], inv_pixel_coords[indices[i]]);
          break;
        }
        default:
          break;
      }

      printf("%d. diff = %g\n", i, diffs.at<double>(0, i));

#if 0
      cv::imshow("fitted", fitted);

      cv::Mat img_ref(images.front().rows, images.front().cols, images.front().type(), cv::Scalar(0, 0, 0));
      FillImage(textures.row(indices[i]), pixel_coords, img_ref);
      cv::imshow("ref", img_ref);

      Mat image_i = images[indices[i]].clone();
      DrawShape(image_i, shapes.row(indices[i]));
      cv::imshow("input", images[indices[i]]);

      cv::putText(warp_back, std::to_string(diffs.at<double>(0, i)), cv::Point(5, 20),
                  cv::FONT_HERSHEY_SIMPLEX, 0.35, cv::Scalar(255, 175, 175));
      cv::imshow("warp_back", warp_back);

      cv::waitKey();
#endif
    }

    cv::Scalar mean_diff, stddev_diff;
    cv::meanStdDev(diffs, mean_diff, stddev_diff);

    cout << mean_diff << ", " << stddev_diff << endl;

    // always create a new inlier directory
    safe_create(fs::path(output_path) / fs::path("inliers"));

    set<int> res;
    for(int i=0;i<nimages;++i) {
      int max_idx = indices[i];

      if(diffs.at<double>(0, i) >= mean_diff[0] + 2 * stddev_diff[0]) {
        cout << "outlier: " << max_idx << endl;
        Mat img_i = images[max_idx].clone();
        DrawShape(img_i, shapes.row(max_idx));
        cv::imwrite(output_path + "/outliers/" + "image" + to_string(max_idx) + ".jpg", img_i * 255);

        Mat img_fitted = fitted_images[i];
        cv::imwrite(output_path + "/outliers/" + "image" + to_string(max_idx) + "_fitted.jpg", img_fitted * 255);

        Mat img(images.front().rows, images.front().cols, images.front().type(), cv::Scalar(0, 0, 0));
        FillImage(reconstructions[i], pixel_coords, img);
        cv::imwrite(output_path + "/outliers/" + "image" + to_string(max_idx) + "_fitted_tex.jpg", img * 255);

        Mat img_ref(images.front().rows, images.front().cols, images.front().type(), cv::Scalar(0, 0, 0));
        FillImage(textures.row(max_idx) + meantexture, pixel_coords, img_ref);
        cv::imwrite(output_path + "/outliers/" + "image" + to_string(max_idx) + "_warped.jpg", img_ref * 255);
      } else {
        res.insert(indices[i]);

        Mat img_i = images[max_idx].clone();
        DrawShape(img_i, shapes.row(max_idx));
        cv::imwrite(output_path + "/inliers/" + "image" + to_string(max_idx) + ".jpg", img_i * 255);

        Mat img_fitted = fitted_images[i];
        cv::imwrite(output_path + "/inliers/" + "image" + to_string(max_idx) + "_fitted.jpg", img_fitted * 255);

        Mat img(images.front().rows, images.front().cols, images.front().type(), cv::Scalar(0, 0, 0));
        FillImage(reconstructions[i], pixel_coords, img);
        cv::imwrite(output_path + "/inliers/" + "image" + to_string(max_idx) + "_fitted_tex.jpg", img * 255);

        Mat img_ref(images.front().rows, images.front().cols, images.front().type(), cv::Scalar(0, 0, 0));
        FillImage(textures.row(max_idx) + meantexture, pixel_coords, img_ref);
        cv::imwrite(output_path + "/inliers/" + "image" + to_string(max_idx) + "_warped.jpg", img_ref * 255);
      }
    }

    cout << "done." << endl;

    return vector<int>(res.begin(), res.end());
  }

  std::vector<int> AAMModel::FindInliers_Iterative(vector<int> indices, Method method) {
    boost::timer::auto_cpu_timer t("Outlier detection finished in %w seconds.\n");
    while(true) {
      int sz = indices.size();
      {
        boost::timer::auto_cpu_timer t("Iteration finished in %w seconds.\n");
        switch (method) {
          case RobustPCA: {
            indices = FindInliers_RPCA(indices);
            break;
          }
          case LeaveOneOut: {
            indices = FindInliers(indices);
            break;
          }
        }

      }
      if(sz == indices.size()) break;
    }
    return indices;
  }

  std::vector<int> AAMModel::FindInliers_RPCA(vector<int> indices) {
    if(indices.empty()) {
      indices.resize(input_images.size());
      std::iota(indices.begin(), indices.end(), 0);
    }

    const int nimages = indices.size();

    set<int> current_set(indices.begin(), indices.end());

    vector<Mat> reconstructions(nimages), fitted_images(nimages);
    Mat diffs(1, nimages, CV_64FC1);

    // Perform RPCA on both shapes and texture
    set<int> set_i = current_set;

    Mat shapes_i(set_i.size(), shapes.cols, shapes.type()),
      normalized_textures_i(set_i.size(), normalized_textures.cols, normalized_textures.type());

    int ridx = 0;
    for(auto j : set_i) {
      shapes_i.row(ridx) = shapes.row(j) * 1;
      normalized_textures_i.row(ridx) = normalized_textures.row(j) * 1;
      ++ridx;
    }
    Mat normalized_textures_i_reshaped = normalized_textures_i.reshape(1);

    auto rpca = [](const cv::Mat& M){
      Eigen::MatrixXd D, A, E;
      D = CVMat2EigenMatrix<double>(M);
      #if 1
      Eigen::MatrixXd DT = D.transpose();
      sp::ml::robust_pca(DT, A, E);
      Eigen::MatrixXd AT = A.transpose();
      return EigenMatrix2CVMat(AT);
      #else
      sp::ml::robust_pca(D, A, E);
      return EigenMatrix2CVMat(A);
      #endif
    };

    {
      boost::timer::auto_cpu_timer t("Matrix recovery finished in %w seconds.\n");
      shapes_i = rpca(shapes_i);
      normalized_textures_i_reshaped = rpca(normalized_textures_i_reshaped);
    }

    // Construct shape and texture model with the provided indices
    cv::PCA shape_model, texture_model;
    {
      boost::timer::auto_cpu_timer t("Shape model constructed in %w seconds.\n");
      shape_model = shape_model(shapes_i, Mat(), CV_PCA_DATA_AS_ROW, 0.98);
    }
    {
      boost::timer::auto_cpu_timer t("Texture model constructed in %w seconds.\n");
      texture_model = texture_model(normalized_textures_i_reshaped,
                                    Mat(),
                                    CV_PCA_DATA_AS_ROW,
                                    0.98);
    }

    PrintShape(texture_model.eigenvectors);

    Mat coeffs(1, texture_model.mean.cols, texture_model.mean.type()), reconstructed;

    //#pragma omp parallel for
    for(int i=0;i<indices.size();++i) {
      Mat vec = textures.row(indices[i]);

      // normalize it
      Mat normalized_vec, beta_i;
      double alpha_i;
      tie(normalized_vec, alpha_i, beta_i) = NormalizeTextureVec(vec, meantexture);
      // subtract meantexture since the PCA model is built on difference
      normalized_vec -= meantexture;

      texture_model.project(normalized_vec.reshape(1), coeffs);
      texture_model.backProject(coeffs, reconstructed);
      reconstructed = reconstructed.reshape(3);

      // unnormalize it
      reconstructions[i] = (reconstructed + meantexture) * alpha_i + beta_i.reshape(3, 1);

      Mat fitted, warp_back;
      switch(metric) {
        case TextureError: {
          diffs.at<double>(0, i) = cv::norm(normalized_vec, reconstructed, cv::NORM_L2);
          break;
        }
        case FittingError: {
          // Warp reconstructed back to image space and compute fitting error using the pixel mask
          double diff_i = 0;
          cout << "warping image ..." << endl;
          fitted = Mat(images.front().rows, images.front().cols, images.front().type(), cv::Scalar(0, 0, 0));
          FillImage(reconstructions[i], pixel_coords, fitted);
          warp_back = WarpImage(fitted, tforms[indices[i]], inv_pixel_mats[indices[i]], inv_pixel_coords[indices[i]]);
          fitted_images[i] = warp_back;
          diffs.at<double>(0, i) = ComputeRMSE(warp_back, images[indices[i]], inv_pixel_coords[indices[i]]);
          break;
        }
        default:
          break;
      }

      printf("%d. diff = %g\n", i, diffs.at<double>(0, i));

    #if 0
      cv::imshow("fitted", fitted);

      cv::Mat img_ref(images.front().rows, images.front().cols, images.front().type(), cv::Scalar(0, 0, 0));
      FillImage(textures.row(indices[i]), pixel_coords, img_ref);
      cv::imshow("ref", img_ref);

      Mat image_i = images[indices[i]].clone();
      DrawShape(image_i, shapes.row(indices[i]));
      cv::imshow("input", images[indices[i]]);

      cv::putText(warp_back, std::to_string(diffs.at<double>(0, i)), cv::Point(5, 20),
                  cv::FONT_HERSHEY_SIMPLEX, 0.35, cv::Scalar(255, 175, 175));
      cv::imshow("warp_back", warp_back);

      cv::waitKey();
    #endif
    }

    cv::Scalar mean_diff, stddev_diff;
    cv::meanStdDev(diffs, mean_diff, stddev_diff);

    cout << mean_diff << ", " << stddev_diff << endl;

    // always create a new inlier directory
    safe_create(fs::path(output_path) / fs::path("inliers"));

    set<int> res;
    for(int i=0;i<nimages;++i) {
      int max_idx = indices[i];

      if(diffs.at<double>(0, i) >= mean_diff[0] + 2 * stddev_diff[0]) {
        cout << "outlier: " << max_idx << endl;
        Mat img_i = images[max_idx].clone();
        DrawShape(img_i, shapes.row(max_idx));
        cv::imwrite(output_path + "/outliers/" + "image" + to_string(max_idx) + ".jpg", img_i * 255);

        Mat img_fitted = fitted_images[i];
        cv::imwrite(output_path + "/outliers/" + "image" + to_string(max_idx) + "_fitted.jpg", img_fitted * 255);

        Mat img(images.front().rows, images.front().cols, images.front().type(), cv::Scalar(0, 0, 0));
        FillImage(reconstructions[i], pixel_coords, img);
        cv::imwrite(output_path + "/outliers/" + "image" + to_string(max_idx) + "_fitted_tex.jpg", img * 255);

        Mat img_ref(images.front().rows, images.front().cols, images.front().type(), cv::Scalar(0, 0, 0));
        FillImage(textures.row(max_idx) + meantexture, pixel_coords, img_ref);
        cv::imwrite(output_path + "/outliers/" + "image" + to_string(max_idx) + "_warped.jpg", img_ref * 255);
      } else {
        res.insert(indices[i]);

        Mat img_i = images[max_idx].clone();
        DrawShape(img_i, shapes.row(max_idx));
        cv::imwrite(output_path + "/inliers/" + "image" + to_string(max_idx) + ".jpg", img_i * 255);

        Mat img_fitted = fitted_images[i];
        cv::imwrite(output_path + "/inliers/" + "image" + to_string(max_idx) + "_fitted.jpg", img_fitted * 255);

        Mat img(images.front().rows, images.front().cols, images.front().type(), cv::Scalar(0, 0, 0));
        FillImage(reconstructions[i], pixel_coords, img);
        cv::imwrite(output_path + "/inliers/" + "image" + to_string(max_idx) + "_fitted_tex.jpg", img * 255);

        Mat img_ref(images.front().rows, images.front().cols, images.front().type(), cv::Scalar(0, 0, 0));
        FillImage(textures.row(max_idx) + meantexture, pixel_coords, img_ref);
        cv::imwrite(output_path + "/inliers/" + "image" + to_string(max_idx) + "_warped.jpg", img_ref * 255);
      }
    }

    cout << "done." << endl;

    return vector<int>(res.begin(), res.end());
  }

}
