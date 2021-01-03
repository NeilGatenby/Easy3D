/**
 * Copyright (C) 2015 by Liangliang Nan (liangliang.nan@gmail.com)
 * https://3d.bk.tudelft.nl/liangliang/
 *
 * This file is part of Easy3D. If it is useful in your research/work,
 * I would be grateful if you show your appreciation by citing it:
 * ------------------------------------------------------------------
 *      Liangliang Nan.
 *      Easy3D: a lightweight, easy-to-use, and efficient C++
 *      library for processing and rendering 3D data. 2018.
 * ------------------------------------------------------------------
 * Easy3D is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License Version 3
 * as published by the Free Software Foundation.
 *
 * Easy3D is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/** ----------------------------------------------------------
 *
 * the code is adapted from libQGLViewer with modifications.
 *		- libQGLViewer (version Version 2.7.1, Nov 17th, 2017)
 * The original code is available at
 * http://libqglviewer.com/
 *
 * libQGLViewer is a C++ library based on Qt that eases the
 * creation of OpenGL 3D viewers.
 *
 *----------------------------------------------------------*/

#include <easy3d/renderer/key_frame_interpolator.h>

#include <fstream>
#include <chrono>
#include <thread>

#include <easy3d/renderer/frame.h>
#include <easy3d/renderer/drawable_lines.h>
#include <easy3d/renderer/primitives.h>
#include <easy3d/renderer/camera.h>   // for drawing the camera path drawables


namespace easy3d {


    // allow to save interpolated frames as keyframes (so I can visualize them)
//#define DEBUG_INTERPOLATED_FRAMES
#ifdef DEBUG_INTERPOLATED_FRAMES
void save_interpolation(const std::vector<easy3d::Frame>& frames) {
    std::string file = "interpolated_frames2.kf";
    std::ofstream output(file.c_str());
    if (output.fail())
        std::cout << "could not open file: " << file << std::endl;
    output << "\tnum_key_frames: " << frames.size() << std::endl;
    for (std::size_t id = 0; id < frames.size(); ++id) {
        output << "\tframe: " << id << std::endl;
        const easy3d::Frame& frame = frames[id];
        output << "\t\tposition: " << frame.position() << std::endl;
        output << "\t\torientation: " << frame.orientation() << std::endl;
    }
}
#endif

    KeyFrameInterpolator::KeyFrameInterpolator(Frame *frame)
            : frame_(frame)
            , fps_(30)
            , interpolation_speed_(1.0f)
            , interpolation_started_(false)
            , last_stopped_index_(0)
            , pathIsValid_(false)
            , path_drawable_(nullptr)
            , cameras_drawable_(nullptr) {
        if (keyframes_.size() < 2)
            return;
    }


    KeyFrameInterpolator::~KeyFrameInterpolator() {
        delete_path();
    }


    void KeyFrameInterpolator::setFrame(Frame *const frame) {
        frame_ = frame;
    }


    void KeyFrameInterpolator::start_interpolation() {
        if (keyframes_.empty())
            return;

        if (!pathIsValid_)
            interpolate();

        // all done in another thread.
        interpolation_started_ = true;
        timer_.set_timeout(0, [this]() {
                               for (int id = last_stopped_index_; id < interpolated_path_.size(); ++id) {
                                   if (timer_.is_stopped()) {
                                       last_stopped_index_ = id;
                                       break;
                                   }
                                   const auto &f = interpolated_path_[id];
                                   frame()->setPositionAndOrientation(f.position(), f.orientation());
                                   // interval in ms. 0.9 for (approximately) compensating the timer overhead
                                   const int interval = interpolation_period() / interpolation_speed() * 0.9f;
                                   std::this_thread::sleep_for(std::chrono::milliseconds(interval));
                                   if (id == interpolated_path_.size() - 1)  // reaches the end frame
                                       last_stopped_index_ = 0;
                                   frame_interpolated.send();
                               }
                               interpolation_stopped.send();
                               interpolation_started_ = false;
                           }
        );
    }


    void KeyFrameInterpolator::stop_interpolation() {
        timer_.stop();
        interpolation_started_ = false;
    }


    void KeyFrameInterpolator::add_keyframe(const Frame &frame, float time) {
        if ((!keyframes_.empty()) && (keyframes_.back()->time() >= time))
            LOG(ERROR) << "time is not monotone";
        else {
            keyframes_.push_back(new KeyFrame(frame, time));
        }

        pathIsValid_ = false;
        last_stopped_index_ = 0; // may not be valid any more
        stop_interpolation();
    }


    void KeyFrameInterpolator::delete_last_keyframe() {
        keyframes_.pop_back();
        pathIsValid_ = false;
        last_stopped_index_ = 0; // may not be valid any more
        stop_interpolation();
    }


    void KeyFrameInterpolator::add_keyframe(const Frame &frame) {
#if 0
        // interval of each consecutive keyframes is 1.0 sec.
        float time = 0.0f;
        if (keyframes_.empty())
            time = 0.0;
        else
            time = keyframes_.back()->time() + 1.0f;
#else
        static float period_reference_distance = 0.0f;
        // interval between first two keyframes is 1.0 sec. and their distant is recorded.
        // The time intervals between other consecutive keyframes are computed relative to this distance.
        float time = 0.0f;
        if (keyframes_.empty())
            time = 0.0;
        else if (keyframes_.size() == 1) {
            time = 1.0f;
            period_reference_distance = distance(keyframes_[0]->position(), frame.position());
        }
        else
            time = keyframes_.back()->time() + 1.0f * distance(keyframes_.back()->position(), frame.position()) / period_reference_distance;
#endif

        add_keyframe(frame, time);
    }


    void KeyFrameInterpolator::delete_path() {
        stop_interpolation();
        for (auto f : keyframes_) {
            delete f;
        }
        keyframes_.clear();
        interpolated_path_.clear();
        pathIsValid_ = false;
        last_stopped_index_ = 0;

        delete path_drawable_;
        path_drawable_ = nullptr;

        delete cameras_drawable_;
        cameras_drawable_ = nullptr;
    }


    void KeyFrameInterpolator::draw_path(const Camera *cam, float camera_width) {
        if (keyframes_.empty())
            return;

        if (!pathIsValid_) {
            if (path_drawable_) {
                delete path_drawable_;
                path_drawable_ = nullptr;
            }
            if (cameras_drawable_) {
                delete cameras_drawable_;
                cameras_drawable_ = nullptr;
            }
            interpolated_path_ = interpolate();
            pathIsValid_ = true;
        }
        if (interpolated_path_.empty()) // interpolation may have failed.
            return;

        if (!path_drawable_) {
            std::vector<vec3> points;

            // the path
            for (std::size_t i = 0; i < interpolated_path_.size() - 1; ++i) {
                points.push_back(interpolated_path_[i].position());
                points.push_back(interpolated_path_[i + 1].position());
            }

            if (points.size() > 1) {
                path_drawable_ = new LinesDrawable;
                path_drawable_->update_vertex_buffer(points);
                path_drawable_->set_uniform_coloring(vec4(1.0f, 0.2f, 0.2f, 1.0f));
                path_drawable_->set_line_width(2);
                path_drawable_->set_impostor_type(LinesDrawable::CYLINDER);
            }
        }

        if (!cameras_drawable_) {
            std::vector<vec3> points;

            // camera representation
            for (std::size_t i = 0; i < keyframes_.size(); ++i) {
                std::vector<vec3> cam_points;
                opengl::prepare_camera(cam_points, camera_width,
                                       static_cast<float>(cam->screenHeight()) / cam->screenWidth());
                const mat4 &m = Frame(keyframes_[i]->position(), keyframes_[i]->orientation()).matrix();
                for (auto &p : cam_points) {
                    points.push_back(m * p);
                }
            }

            if (points.size() > 1) {
                cameras_drawable_ = new LinesDrawable;
                cameras_drawable_->update_vertex_buffer(points);
                cameras_drawable_->set_uniform_coloring(vec4(0.0f, 0.0f, 1.0f, 1.0f));
                cameras_drawable_->set_line_width(2);
            }
        }

        if (path_drawable_)
            path_drawable_->draw(cam);
        if (cameras_drawable_)
            cameras_drawable_->draw(cam);
    }


    bool KeyFrameInterpolator::save_keyframes(const std::string &file_name) const {
        std::ofstream output(file_name.c_str());
        if (output.fail()) {
            LOG(ERROR) << "unable to open \'" << file_name << "\'";
            return false;
        }

        output << "\tnum_key_frames: " << keyframes_.size() << std::endl;

        for (std::size_t id = 0; id < keyframes_.size(); ++id) {
            output << "\tframe: " << id << std::endl;
            const KeyFrame* frame = keyframes_[id];
            output << "\t\tposition: " << frame->position() << std::endl;
            output << "\t\torientation: " << frame->orientation() << std::endl;
        }

        return keyframes_.size() > 0;
    }


    bool KeyFrameInterpolator::read_keyframes(const std::string &file_name) {
        std::ifstream input(file_name.c_str());
        if (input.fail()) {
            LOG(ERROR) << "unable to open \'" << file_name << "\'";
            return false;
        }

        // clean
        delete_path();

        // load path from file
        std::string dummy;
        int num_key_frames;
        input >> dummy >> num_key_frames;
        for (int j = 0; j < num_key_frames; ++j) {
            int frame_id;
            input >> dummy >> frame_id;
            vec3 pos;
            quat orient;
            input >> dummy >> pos >> dummy >> orient;
            add_keyframe(Frame(pos, orient));
        }

        return keyframes_.size() > 0;
    }


    void KeyFrameInterpolator::updateModifiedFrameValues(std::vector<KeyFrame*>& keyFrames) {
        quat prevQ = keyFrames.front()->orientation();
        KeyFrame *kf = nullptr;
        for (std::size_t i = 0; i < keyFrames.size(); ++i) {
            kf = keyFrames.at(i);
            kf->flipOrientationIfNeeded(prevQ);
            prevQ = kf->orientation();
        }

        KeyFrame *prev = keyFrames.front();
        kf = keyFrames.front();
        std::size_t index = 1;
        while (kf) {
            KeyFrame *next = (index < keyFrames.size()) ? keyFrames.at(index) : nullptr;
            index++;
            if (next)
                kf->computeTangent(prev, next);
            else
                kf->computeTangent(prev, kf);
            prev = kf;
            kf = next;
        }
    }


    Frame KeyFrameInterpolator::keyframe(std::size_t index) const {
        const KeyFrame *const kf = keyframes_.at(index);
        return Frame(kf->position(), kf->orientation());
    }


    float KeyFrameInterpolator::keyframe_time(std::size_t index) const {
        return keyframes_.at(index)->time();
    }


    float KeyFrameInterpolator::duration() const {
        return lastTime() - firstTime();
    }


    float KeyFrameInterpolator::firstTime() const {
        if (keyframes_.empty())
            return 0.0f;
        else
            return keyframes_.front()->time();
    }


    float KeyFrameInterpolator::lastTime() const {
        if (keyframes_.empty())
            return 0.0f;
        else
            return keyframes_.back()->time();
    }


    void KeyFrameInterpolator::set_interpolation_speed(float speed) {
        interpolation_speed_ = speed;
        pathIsValid_ = false;
    }


    void KeyFrameInterpolator::set_frame_rate(int fps) {
        fps_ = fps;
        pathIsValid_ = false;
    }


#ifndef DOXYGEN

    //////////// KeyFrame private class implementation /////////
    KeyFrameInterpolator::KeyFrame::KeyFrame(const Frame &fr, float t)
            : time_(t) {
        p_ = fr.position();
        q_ = fr.orientation();
    }

    void KeyFrameInterpolator::KeyFrame::computeTangent(const KeyFrame *const prev, const KeyFrame *const next) {
#if 1
        // distance(prev, cur) and distance(cur, next) can have a big difference.
        // we compensate this
        float sd_prev = distance2(prev->position(), position());
        float sd_next = distance2(next->position(), position());
        if (sd_prev < sd_next) {
            vec3 new_next = position() + (next->position() - position()).normalize() * std::sqrt(sd_prev);
            tgP_ = 0.5f * (new_next - prev->position());
        }
        else {
            vec3 new_prev = position() +(prev->position() - position()).normalize() * std::sqrt(sd_next);
            tgP_ = 0.5f * (next->position() - new_prev);
        }
#else
        tgP_ = 0.5f * (next->position() - prev->position());
#endif
        tgQ_ = quat::squad_tangent(prev->orientation(), q_, next->orientation());
    }

    void KeyFrameInterpolator::KeyFrame::flipOrientationIfNeeded(const quat &prev) {
        if (quat::dot(prev, q_) < 0.0f)
            q_.negate();
    }


    void KeyFrameInterpolator::getRelatedKeyFramesForTime(float time, const std::vector<KeyFrame*>& keyFrames, std::vector<KeyFrame *>::const_iterator *relatedFrames) const {
        // Assertion: times are sorted in monotone order.
        // Assertion: keyframes_ is not empty

        // TODO: Special case for loops when closed path is implemented !!
        // Recompute everything from scratch
        relatedFrames[1] = keyFrames.begin(); // points to the first one

        while ((*relatedFrames[1])->time() > time) {
            if (relatedFrames[1] == keyFrames.begin()) // already the first one
                break;
            --relatedFrames[1]; // points to the previous one
        }

        relatedFrames[2] = relatedFrames[1];
        while ((*relatedFrames[2])->time() < time) {
            if (*relatedFrames[2] == keyFrames.back()) // already the last one
                break;
            ++relatedFrames[2];// points to the next one
        }

        relatedFrames[1] = relatedFrames[2];
        if ((relatedFrames[1] != keyFrames.begin()) && (time < (*relatedFrames[2])->time()))
            --relatedFrames[1];

        relatedFrames[0] = relatedFrames[1];
        if (relatedFrames[0] != keyFrames.begin())
            --relatedFrames[0];

        relatedFrames[3] = relatedFrames[2];

        if ((*relatedFrames[3]) != keyFrames.back()) // has next
            ++relatedFrames[3];
    }


    void KeyFrameInterpolator::computeSpline(const std::vector<KeyFrame*>::const_iterator* relatedFrames, vec3& v1, vec3& v2) const {
        const vec3 &delta = (*relatedFrames[2])->position() - (*relatedFrames[1])->position();
        v1 = 3.0 * delta - 2.0 * (*relatedFrames[1])->tgP() - (*relatedFrames[2])->tgP();
        v2 = -2.0 * delta + (*relatedFrames[1])->tgP() + (*relatedFrames[2])->tgP();
    }


    const std::vector<Frame>& KeyFrameInterpolator::interpolate() {
        if (pathIsValid_)
            return interpolated_path_;

        LOG_IF(INFO, keyframes_.size() > 2) << "interpolating " << keyframes_.size() << " keyframes...";
        do_interpolate(interpolated_path_, keyframes_);
        LOG_IF(INFO, keyframes_.size() > 2) << "keyframe interpolation done, " << interpolated_path_.size() << " frames";

        // more iterations do not provide further improvement
        const int num_iter = 1;
        for (int i = 0; i < num_iter; ++i)
            smooth(interpolated_path_);

        pathIsValid_ = true;
        return interpolated_path_;
    }


//    int idx = 0;

    void KeyFrameInterpolator::do_interpolate(std::vector<Frame>& frames, std::vector<KeyFrame*>& keyFrames) {
        frames.clear();
        if (keyFrames.empty())
            return;

//        std::ofstream  output ("tmp-iner-" + std::to_string(idx++) + ".xyz");

        updateModifiedFrameValues(keyFrames);

        const float interval = interpolation_speed() * interpolation_period() / 1000.0f;
        for (float time = firstTime(); time < lastTime() + interval; time += interval) {
            std::vector<KeyFrame *>::const_iterator relatedFrames[4];
            getRelatedKeyFramesForTime(time, keyFrames, relatedFrames);

            vec3 v1, v2;
            computeSpline(relatedFrames, v1, v2);

            float alpha = 0.0f;
            const float dt = (*relatedFrames[2])->time() - (*relatedFrames[1])->time();
            if (std::abs(dt) < epsilon<float>())
                alpha = 0.0f;
            else
                alpha = (time - (*relatedFrames[1])->time()) / dt;

#if 0   // linear interpolation - not smooth
            vec3 pos = alpha*((*relatedFrames[2])->position()) + (1.0f-alpha)*((*relatedFrames[1])->position());
            quat a = (*relatedFrames[1])->orientation();
            quat b = (*relatedFrames[2])->orientation();
            quat q = quat(
                    alpha * b[0] + (1.0f -alpha) * a[0],
                    alpha * b[1] + (1.0f -alpha) * a[1],
                    alpha * b[2] + (1.0f -alpha) * a[2],
                    alpha * b[3] + (1.0f -alpha) * a[3]
            );
            q.normalize();

#else   // spline interpolation
            const vec3 pos =
                    (*relatedFrames[1])->position() + alpha * ((*relatedFrames[1])->tgP() + alpha * (v1 + alpha * v2));
            const quat q = quat::squad((*relatedFrames[1])->orientation(), (*relatedFrames[1])->tgQ(),
                                       (*relatedFrames[2])->tgQ(), (*relatedFrames[2])->orientation(), alpha);
#endif
            Frame f;
            f.setPosition(pos);
            f.setOrientation(q);
            frames.push_back(f);

//            output << pos << std::endl;
        }

#ifdef DEBUG_INTERPOLATED_FRAMES
        save_interpolation(frames);
#endif
    }


    void KeyFrameInterpolator::smooth(std::vector<Frame>& frames) {
        if (frames.size() < 2)
            return;

        // use the interpolated frames as keyframes -> another round interpolation

        const float interval = interpolation_speed() * interpolation_period() / 1000.0f;
        float period_reference_distance = 0.0f;
        std::vector<KeyFrame *> as_key_frames;
        for (std::size_t i = 0; i < frames.size(); ++i) {
            // interval between first two keyframes is 1.0 sec. and their distant is recorded.
            // The time intervals between other consecutive keyframes are computed relative to this distance.
            float time = 0.0f;
            if (as_key_frames.empty())
                time = 0.0;
            else if (as_key_frames.size() == 1) { // all equal intervals
                time = interval;
                period_reference_distance = distance(as_key_frames[0]->position(), frames[i].position());
            } else
                time = as_key_frames.back()->time() +
                        interval * distance(as_key_frames.back()->position(), frames[i].position()) / period_reference_distance;

            as_key_frames.push_back(new KeyFrame(frames[i], time));
        }

        // adjust the period, using the ratio of the two period
        float ratio = duration() / (as_key_frames.back()->time() - as_key_frames.front()->time());
        if (is_nan(ratio)) {
            LOG(ERROR) << "duration is 0";
        }
        else {
            for (auto &f : as_key_frames)
                f->set_time(f->time() * ratio);

            do_interpolate(frames, as_key_frames);
        }

        // clean
        for (auto f : as_key_frames)
            delete f;
    }


}


#endif //DOXYGEN
